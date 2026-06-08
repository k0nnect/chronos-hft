// on-disk & on-wire framing structures for offline pcap replay.
//
// a libpcap savefile is a 24-byte global header followed by a sequence of
// records, each a 16-byte record header + the captured link-layer frame. for an
// ethernet multicast capture the frame nests as:
//
//   ethernet(14) -> ipv4(20+) -> udp(8) -> payload (our itch-like stream)
//
// the packed structs below are the authoritative description of each layer's
// byte layout; the static_asserts pin their exact wire sizes so a stray compiler
// padding byte can never silently shift a field. as in itch_protocol.hpp the
// reader never dereferences a multi-byte field *through* these structs (that
// would be an alignment & strict-aliasing hazard on an mmap'd buffer) -- it reads
// every multi-byte value with an explicit, byte-exact load helper instead. the
// structs exist for sizeof / offsetof & documentation.
//
// two endianness conventions coexist in one file & must not be confused:
//   * the pcap global/record headers are written in the *host* byte order of the
//     machine that captured the trace; the global magic number tells us whether
//     that matches us (read straight) or is byte-swapped (bswap on read).
//   * the ip & udp headers are always *network* byte order (big-endian), so they
//     are read with load_be regardless of the capture host.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hft/core/byte_order.hpp"

namespace hft::pcap {

// ---- magic numbers & protocol constants -----------------------------------

// classic libpcap global-header magic, microsecond timestamp resolution. read as
// this value => the trace's byte order matches ours (no swap). read as the
// byte-reversed value => the trace is foreign-endian (swap every meta field).
inline constexpr std::uint32_t magic_micros         = 0xA1B2C3D4u;
inline constexpr std::uint32_t magic_micros_swapped = 0xD4C3B2A1u;
// pcap-ng-era nanosecond timestamp resolution variant (still a classic savefile).
inline constexpr std::uint32_t magic_nanos          = 0xA1B23C4Du;
inline constexpr std::uint32_t magic_nanos_swapped  = 0x4D3CB2A1u;

inline constexpr std::uint32_t linktype_ethernet = 1;       // DLT_EN10MB
inline constexpr std::uint16_t ethertype_ipv4    = 0x0800;  // ip over ethernet
inline constexpr std::uint8_t  ip_proto_udp      = 17;      // udp inside ipv4

// ---- packed layout structs (sizeof / offsetof only) -----------------------

#pragma pack(push, 1)

// 24-byte savefile global header. one per file, at offset 0.
struct global_header {
    std::uint32_t magic_number;   // byte-order & ts-resolution sentinel
    std::uint16_t version_major;  // typically 2
    std::uint16_t version_minor;  // typically 4
    std::int32_t  thiszone;       // gmt offset of timestamps (seconds)
    std::uint32_t sigfigs;        // timestamp accuracy (in practice 0)
    std::uint32_t snaplen;        // max captured bytes per packet
    std::uint32_t network;        // link-layer type (linktype_ethernet here)
};
static_assert(sizeof(global_header) == 24, "pcap global header wire size");

// 16-byte per-record header. precedes each captured frame. `incl_len` is the
// number of frame bytes actually stored in the file -- the exact amount the
// cursor must step over to reach the next record header.
struct record_header {
    std::uint32_t ts_sec;    // capture time, whole seconds
    std::uint32_t ts_frac;   // micro- or nanoseconds (per the global magic)
    std::uint32_t incl_len;  // bytes of this frame present in the file
    std::uint32_t orig_len;  // original on-wire frame length (>= incl_len)
};
static_assert(sizeof(record_header) == 16, "pcap record header wire size");

// 14-byte ethernet ii header. `ethertype` is big-endian on the wire.
struct ethernet_header {
    std::uint8_t  dst_mac[6];
    std::uint8_t  src_mac[6];
    std::uint16_t ethertype;  // big-endian; ethertype_ipv4 for our feed
};
static_assert(sizeof(ethernet_header) == 14, "ethernet ii header wire size");

// 20-byte ipv4 fixed header (options, if any, follow & extend it). the true
// header length is (version_ihl & 0x0F) * 4 bytes -- never assume 20. all
// multi-byte fields are big-endian.
struct ipv4_header {
    std::uint8_t  version_ihl;     // high nibble: version(4); low nibble: ihl (words)
    std::uint8_t  dscp_ecn;        // differentiated services / ecn
    std::uint16_t total_length;    // header + payload, big-endian
    std::uint16_t identification;  // big-endian
    std::uint16_t flags_fragment;  // flags + fragment offset, big-endian
    std::uint8_t  ttl;             // time to live
    std::uint8_t  protocol;        // ip_proto_udp for our feed
    std::uint16_t header_checksum; // big-endian (not validated on replay)
    std::uint32_t src_addr;        // big-endian
    std::uint32_t dst_addr;        // big-endian (the multicast group)
};
static_assert(sizeof(ipv4_header) == 20, "ipv4 fixed header wire size");

// 8-byte udp header. all fields big-endian. `length` covers this header + the
// payload that follows.
struct udp_header {
    std::uint16_t src_port;  // big-endian
    std::uint16_t dst_port;  // big-endian
    std::uint16_t length;    // header(8) + payload, big-endian
    std::uint16_t checksum;  // big-endian (0 = not computed; not validated)
};
static_assert(sizeof(udp_header) == 8, "udp header wire size");

#pragma pack(pop)

// fixed byte spans the cursor steps over. kept as named constants so the pointer
// arithmetic in pcap_reader reads as layer names, not magic numbers.
inline constexpr std::size_t global_header_size = sizeof(global_header);   // 24
inline constexpr std::size_t record_header_size = sizeof(record_header);   // 16
inline constexpr std::size_t ethernet_size      = sizeof(ethernet_header); // 14
inline constexpr std::size_t ipv4_min_size      = sizeof(ipv4_header);     // 20
inline constexpr std::size_t udp_size           = sizeof(udp_header);      //  8

// byte offsets used by the byte-exact readers below (independent of struct
// padding, which the static_asserts already prove is absent, but explicit is
// safer than trusting offsetof on a packed aggregate).
inline constexpr std::size_t gh_off_magic   = 0;
inline constexpr std::size_t gh_off_network = 20;
inline constexpr std::size_t rh_off_incl    = 8;   // incl_len within record_header
inline constexpr std::size_t eth_off_type   = 12;  // ethertype within ethernet_header
inline constexpr std::size_t udp_off_length = 4;   // length within udp_header

// ---- byte-exact field readers ---------------------------------------------

// read a host-order pcap meta field (global/record header). `swapped` is decided
// once, from the global magic. a single memcpy keeps the read alignment-safe on
// the mmap'd region; the optional bswap is one instruction.
[[nodiscard]] inline std::uint32_t read_meta_u32(const std::uint8_t* p, bool swapped) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return swapped ? bswap(v) : v;
}

[[nodiscard]] inline std::uint16_t read_meta_u16(const std::uint8_t* p, bool swapped) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return swapped ? bswap(v) : v;
}

// classify a global-header magic. returns false for an unrecognised file; on
// success reports whether meta fields are byte-swapped relative to us & whether
// record timestamps are nanosecond (vs microsecond) resolution.
[[nodiscard]] inline bool classify_magic(std::uint32_t magic, bool& swapped,
                                         bool& nanos) noexcept {
    switch (magic) {
        case magic_micros:         swapped = false; nanos = false; return true;
        case magic_micros_swapped: swapped = true;  nanos = false; return true;
        case magic_nanos:          swapped = false; nanos = true;  return true;
        case magic_nanos_swapped:  swapped = true;  nanos = true;  return true;
        default:                   return false;
    }
}

}  // namespace hft::pcap
