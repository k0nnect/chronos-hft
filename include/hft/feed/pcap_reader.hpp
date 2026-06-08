// offline pcap replay: mmap the file once, then walk it zero-copy.
//
// `PcapReader` maps a `.pcap` trace read-only into the address space (a single
// mmap, no per-packet allocation or copy) & validates the global header.
// `PcapPacketCursor` iterates the records, peeling ethernet -> ipv4 -> udp off
// each captured frame & handing back a pointer + length straight into the mapped
// pages -- the raw udp payload, which for our captures is the itch-like wire
// stream the existing `itch::frame_cursor` / `decode` already understand.
//
// the cursor never copies, never allocates & is noexcept. the one invariant that
// keeps it from walking off the mapping is in `next()`: it advances its internal
// pointer by *exactly* record_header_size + incl_len for every record -- the byte
// count the record header declares -- whether or not that record yielded a udp
// payload. every layer step is bounds-checked against the end of the mapping
// before a single field is read, so a truncated or malformed trace ends iteration
// cleanly rather than faulting.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hft/core/compiler.hpp"
#include "hft/feed/pcap_structures.hpp"

namespace hft {

// the result of peeling one udp datagram out of a captured frame: a view into
// the mmap'd payload plus the lightweight metadata a feed consumer may want. the
// payload pointer is valid for as long as the owning PcapReader keeps the file
// mapped.
struct udp_payload_view {
    const std::uint8_t* data = nullptr;  // first payload byte (into the mapping)
    std::size_t         len  = 0;        // payload length in bytes
    std::uint16_t       src_port = 0;    // host order
    std::uint16_t       dst_port = 0;    // host order (the multicast feed port)
    std::uint32_t       ts_sec   = 0;    // capture time, whole seconds
    std::uint32_t       ts_frac  = 0;    // sub-second ticks (micro/nano per file)
};

// zero-copy forward iterator over the udp payloads of a mapped pcap trace.
// constructed by PcapReader::cursor(); cheap to copy (three pointers + a flag).
class PcapPacketCursor {
public:
    PcapPacketCursor(const std::uint8_t* begin, const std::uint8_t* end,
                     bool swapped) noexcept
        : cur_(begin), end_(end), swapped_(swapped) {}

    // advance to the next ipv4/udp datagram, skipping any record that is not one
    // (arp, ipv6, non-udp ip, ...). returns false & leaves `out` unchanged once
    // the trace is exhausted or only a truncated record remains. the pointer is
    // stepped by the full declared record length on every iteration, so skipped
    // records never desynchronise the walk.
    [[nodiscard]] hft_hot bool next(udp_payload_view& out) noexcept {
        while (true) {
            // need a full record header to know how far this record reaches.
            if (cur_ + pcap::record_header_size > end_) [[unlikely]] {
                return false;
            }
            const std::uint32_t incl_len =
                pcap::read_meta_u32(cur_ + pcap::rh_off_incl, swapped_);

            const std::uint8_t* frame = cur_ + pcap::record_header_size;
            // a record whose body straddles the end of the mapping is truncated;
            // stop rather than read past the mapped pages.
            if (incl_len > static_cast<std::size_t>(end_ - frame)) [[unlikely]] {
                return false;
            }

            // capture the advance target up front: whatever happens while parsing
            // the frame, the next record starts exactly here.
            const std::uint8_t* next_record = frame + incl_len;
            const std::uint32_t ts_sec  = pcap::read_meta_u32(cur_, swapped_);
            const std::uint32_t ts_frac = pcap::read_meta_u32(cur_ + 4, swapped_);

            const bool got = extract_udp(frame, incl_len, ts_sec, ts_frac, out);
            cur_ = next_record;  // <-- the invariant: advance by the exact record size
            if (got) {
                return true;
            }
            // not a udp/ipv4 frame: fall through to the next record.
        }
    }

    [[nodiscard]] bool exhausted() const noexcept {
        return cur_ + pcap::record_header_size > end_;
    }

private:
    // peel ethernet -> ipv4 -> udp off one captured frame of `frame_len` bytes.
    // every step checks it has the bytes it is about to read, all relative to the
    // frame (never the raw mapping), so it cannot over-read. returns false for any
    // frame that is not a well-formed ipv4/udp datagram.
    [[nodiscard]] hft_always_inline bool extract_udp(const std::uint8_t* frame,
                                                     std::size_t frame_len,
                                                     std::uint32_t ts_sec,
                                                     std::uint32_t ts_frac,
                                                     udp_payload_view& out) const noexcept {
        // --- ethernet (14 bytes, fixed) ---
        if (frame_len < pcap::ethernet_size) {
            return false;
        }
        const std::uint16_t ethertype = load_be<std::uint16_t>(frame + pcap::eth_off_type);
        if (ethertype != pcap::ethertype_ipv4) {
            return false;  // not ip (e.g. arp / vlan / ipv6) -- skip this record
        }

        // --- ipv4 (variable: ihl * 4 bytes) ---
        const std::uint8_t*  ip        = frame + pcap::ethernet_size;
        const std::size_t    ip_avail  = frame_len - pcap::ethernet_size;
        if (ip_avail < pcap::ipv4_min_size) {
            return false;
        }
        const std::uint8_t version = static_cast<std::uint8_t>(ip[0] >> 4);
        const std::size_t  ihl_words = static_cast<std::size_t>(ip[0] & 0x0F);
        const std::size_t  ip_hdr_len = ihl_words * 4u;
        // a legal ipv4 header is 20..60 bytes & must fit inside the captured frame.
        if (version != 4 || ip_hdr_len < pcap::ipv4_min_size || ip_hdr_len > ip_avail) {
            return false;
        }
        if (ip[9] != pcap::ip_proto_udp) {
            return false;  // not udp -- skip
        }

        // --- udp (8 bytes, fixed) ---
        const std::uint8_t* udp       = ip + ip_hdr_len;
        const std::size_t   udp_avail = ip_avail - ip_hdr_len;
        if (udp_avail < pcap::udp_size) {
            return false;
        }
        const std::uint16_t udp_len = load_be<std::uint16_t>(udp + pcap::udp_off_length);

        // payload follows the 8-byte udp header. trust the smaller of (a) what the
        // udp length field claims & (b) what is actually captured, so a bogus
        // length can never push the view past the mapped frame.
        const std::size_t captured_payload = udp_avail - pcap::udp_size;
        std::size_t       payload_len      = captured_payload;
        if (udp_len >= pcap::udp_size) {
            const std::size_t declared = static_cast<std::size_t>(udp_len) - pcap::udp_size;
            if (declared < payload_len) {
                payload_len = declared;
            }
        }

        out.data     = udp + pcap::udp_size;
        out.len      = payload_len;
        out.src_port = load_be<std::uint16_t>(udp + 0);
        out.dst_port = load_be<std::uint16_t>(udp + 2);
        out.ts_sec   = ts_sec;
        out.ts_frac  = ts_frac;
        return true;
    }

    const std::uint8_t* cur_;      // next record header to read
    const std::uint8_t* end_;      // one past the last mapped byte
    bool                swapped_;  // meta fields are foreign-endian
};

// owns a read-only memory mapping of a pcap savefile for its lifetime. open() &
// close() are the cold setup/teardown path (they touch the kernel); everything
// the hot loop calls -- cursor(), the cursor's next() -- is allocation-free,
// copy-free & noexcept. move-only: the mapping has a single owner.
class PcapReader {
public:
    PcapReader() noexcept = default;
    ~PcapReader() noexcept { close(); }

    PcapReader(const PcapReader&)            = delete;
    PcapReader& operator=(const PcapReader&) = delete;

    PcapReader(PcapReader&& other) noexcept { move_from(other); }
    PcapReader& operator=(PcapReader&& other) noexcept {
        if (this != &other) {
            close();
            move_from(other);
        }
        return *this;
    }

    // map `path` & validate its global header. returns false (leaving the reader
    // closed) if the file cannot be opened/mapped, is shorter than a global
    // header, or carries an unrecognised magic. never throws.
    [[nodiscard]] bool open(const char* path) noexcept;

    // unmap & release. idempotent; also run by the destructor.
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return base_ != nullptr; }

    // a fresh cursor positioned at the first record (just past the global header).
    [[nodiscard]] PcapPacketCursor cursor() const noexcept {
        return PcapPacketCursor(base_ + pcap::global_header_size, base_ + size_, swapped_);
    }

    [[nodiscard]] const std::uint8_t* data() const noexcept { return base_; }
    [[nodiscard]] std::size_t         size() const noexcept { return size_; }
    [[nodiscard]] bool                byte_swapped() const noexcept { return swapped_; }
    [[nodiscard]] bool                nanosecond() const noexcept { return nanos_; }
    [[nodiscard]] std::uint32_t       link_type() const noexcept { return link_type_; }

private:
    void move_from(PcapReader& other) noexcept {
        base_      = other.base_;
        size_      = other.size_;
        swapped_   = other.swapped_;
        nanos_     = other.nanos_;
        link_type_ = other.link_type_;
        other.base_      = nullptr;
        other.size_      = 0;
        other.swapped_   = false;
        other.nanos_     = false;
        other.link_type_ = 0;
    }

    std::uint8_t* base_      = nullptr;  // start of the mapping (global header)
    std::size_t   size_      = 0;        // mapped length in bytes
    bool          swapped_   = false;    // meta fields are foreign-endian
    bool          nanos_     = false;    // record timestamps are nanosecond-res
    std::uint32_t link_type_ = 0;        // global header network field
};

}  // namespace hft
