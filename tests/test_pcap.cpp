// offline pcap replay tests.
//
// these build a mock libpcap savefile *in memory* -- a real global header, real
// 16-byte record headers, & real ethernet/ipv4/udp framing wrapped around our
// itch-like payloads -- write it to a temp file, then map it back through
// PcapReader & assert that the zero-copy cursor peels every udp datagram out &
// the existing itch decoder reads every embedded message bit-for-bit.
//
// the framing is deliberately varied to stress the pointer math the cursor lives
// or dies by: single- & multi-frame payloads, an ipv4 header carrying options
// (ihl=6, so the udp header sits 24 bytes in, not 20), an arp frame & a tcp
// datagram that must be skipped without desynchronising the walk, & a truncated
// trailing record that must end iteration cleanly instead of faulting.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.hpp"
#include "hft/book/apply.hpp"
#include "hft/book/order_book.hpp"
#include "hft/core/byte_order.hpp"
#include "hft/feed/feed_handler.hpp"
#include "hft/feed/itch_generator.hpp"
#include "hft/feed/itch_parser.hpp"
#include "hft/feed/market_event.hpp"
#include "hft/feed/pcap_reader.hpp"
#include "hft/feed/pcap_structures.hpp"

using namespace hft;

namespace {

constexpr std::uint16_t kStock = 7;
constexpr price_t       kBase  = 1'000'000;
constexpr std::uint16_t kPort  = 30001;  // the udp multicast feed port

// ---- little byte-buffer builders (tests may allocate freely) ---------------

void put_u32_host(std::vector<std::uint8_t>& b, std::uint32_t v) {
    std::uint8_t t[4];
    std::memcpy(t, &v, sizeof(t));  // host order: matches a non-swapped pcap magic
    b.insert(b.end(), t, t + sizeof(t));
}

template <typename T>
void put_be(std::vector<std::uint8_t>& b, T v) {
    std::uint8_t t[sizeof(T)];
    store_be<T>(t, v);
    b.insert(b.end(), t, t + sizeof(T));
}

void put_bytes(std::vector<std::uint8_t>& b, const std::uint8_t* p, std::size_t n) {
    b.insert(b.end(), p, p + n);
}

// 14-byte ethernet ii header with the given ethertype (big-endian on the wire).
void append_ethernet(std::vector<std::uint8_t>& frame, std::uint16_t ethertype) {
    const std::uint8_t dst[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};  // ip multicast mac
    const std::uint8_t src[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    put_bytes(frame, dst, sizeof(dst));
    put_bytes(frame, src, sizeof(src));
    put_be<std::uint16_t>(frame, ethertype);
}

// build a full ethernet/ipv4 frame carrying `protocol`. `ihl_words` selects the
// ipv4 header length (5 => 20 bytes, 6 => 24 bytes of options) so the test can
// exercise the variable-length-header advance. for udp the payload is the udp
// header + `payload`; for any other protocol the payload bytes follow the ip
// header directly (enough to look like a real datagram we must skip).
std::vector<std::uint8_t> make_ip_frame(std::uint8_t protocol, std::size_t ihl_words,
                                        std::uint16_t src_port, std::uint16_t dst_port,
                                        const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> frame;
    append_ethernet(frame, pcap::ethertype_ipv4);

    const std::size_t ip_hdr_len = ihl_words * 4u;
    const bool        is_udp     = (protocol == pcap::ip_proto_udp);
    const std::size_t l4_len     = is_udp ? (pcap::udp_size + payload.size()) : payload.size();
    const std::size_t total_len  = ip_hdr_len + l4_len;

    // ipv4 fixed header (20 bytes), all multi-byte fields big-endian.
    frame.push_back(static_cast<std::uint8_t>(0x40 | (ihl_words & 0x0F)));  // version 4 + ihl
    frame.push_back(0x00);                                                  // dscp/ecn
    put_be<std::uint16_t>(frame, static_cast<std::uint16_t>(total_len));    // total length
    put_be<std::uint16_t>(frame, 0x1234);                                   // identification
    put_be<std::uint16_t>(frame, 0x4000);                                   // flags: don't fragment
    frame.push_back(64);                                                    // ttl
    frame.push_back(protocol);                                              // protocol
    put_be<std::uint16_t>(frame, 0x0000);                                   // checksum (unchecked)
    put_be<std::uint32_t>(frame, 0xC0A80001u);                             // src 192.168.0.1
    put_be<std::uint32_t>(frame, 0xEF000001u);                             // dst 239.0.0.1 (mcast)

    // ipv4 options padding to reach ihl_words*4 bytes (e.g. one 4-byte nop block).
    for (std::size_t i = pcap::ipv4_min_size; i < ip_hdr_len; ++i) {
        frame.push_back(0x00);
    }

    if (is_udp) {
        put_be<std::uint16_t>(frame, src_port);
        put_be<std::uint16_t>(frame, dst_port);
        put_be<std::uint16_t>(frame, static_cast<std::uint16_t>(pcap::udp_size + payload.size()));
        put_be<std::uint16_t>(frame, 0x0000);  // udp checksum (0 = not computed)
    }
    put_bytes(frame, payload.data(), payload.size());
    return frame;
}

// a raw (non-ip) ethernet frame, e.g. arp, that the cursor must skip.
std::vector<std::uint8_t> make_raw_frame(std::uint16_t ethertype, std::size_t filler) {
    std::vector<std::uint8_t> frame;
    append_ethernet(frame, ethertype);
    for (std::size_t i = 0; i < filler; ++i) {
        frame.push_back(static_cast<std::uint8_t>(0xA0 + (i & 0x0F)));
    }
    return frame;
}

// append one pcap record: 16-byte host-order header (incl_len = frame size) then
// the captured frame bytes verbatim.
void append_record(std::vector<std::uint8_t>& file, std::uint32_t ts_sec, std::uint32_t ts_frac,
                   const std::vector<std::uint8_t>& frame) {
    put_u32_host(file, ts_sec);
    put_u32_host(file, ts_frac);
    put_u32_host(file, static_cast<std::uint32_t>(frame.size()));  // incl_len
    put_u32_host(file, static_cast<std::uint32_t>(frame.size()));  // orig_len
    put_bytes(file, frame.data(), frame.size());
}

// 24-byte global header: classic microsecond magic (non-swapped on this host),
// version 2.4, ethernet link type.
void append_global_header(std::vector<std::uint8_t>& file) {
    put_u32_host(file, pcap::magic_micros);
    put_be<std::uint16_t>(file, 2);  // version_major (value, endianness irrelevant)
    put_be<std::uint16_t>(file, 4);  // version_minor
    put_u32_host(file, 0);                       // thiszone
    put_u32_host(file, 0);                       // sigfigs
    put_u32_host(file, 65535);                   // snaplen
    put_u32_host(file, pcap::linktype_ethernet);  // network
}

// a unique scratch path in /tmp for this process.
std::string scratch_path() {
    return std::string("/tmp/chronos_pcap_test_") + std::to_string(::getpid()) + ".pcap";
}

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const std::size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return n == bytes.size();
}

// ---------------------------------------------------------------------------
// the main end-to-end replay: mixed framing, every udp payload decoded.
// ---------------------------------------------------------------------------
void pcap_replay_decodes_every_packet() {
    // --- build the embedded itch payloads ---
    std::vector<std::uint8_t> p1;  // 1 frame: add bid
    itch::encode_add(p1, kStock, /*ts=*/1, /*ref=*/100, side::bid, /*qty=*/250, kBase + 10);

    std::vector<std::uint8_t> p2;  // 2 frames: add ask, add bid
    itch::encode_add(p2, kStock, 2, 101, side::ask, 300, kBase + 12);
    itch::encode_add(p2, kStock, 3, 102, side::bid, 150, kBase + 9);

    std::vector<std::uint8_t> p3;  // 2 frames behind ipv4 options (ihl=6)
    itch::encode_execute(p3, kStock, 4, 100, /*executed=*/50, /*match=*/9001);
    itch::encode_delete(p3, kStock, 5, 102);

    std::vector<std::uint8_t> p4;  // 1 frame: trade
    itch::encode_trade(p4, kStock, 6, 101, side::ask, 75, kBase + 12, /*match=*/9002);

    // a tcp datagram whose payload *looks* like itch but must never be decoded.
    std::vector<std::uint8_t> tcp_payload;
    itch::encode_add(tcp_payload, kStock, 999, 7777, side::bid, 9999, kBase + 999);

    // --- assemble the savefile ---
    std::vector<std::uint8_t> file;
    append_global_header(file);
    append_record(file, 1000, 1, make_ip_frame(pcap::ip_proto_udp, 5, kPort, kPort, p1));
    append_record(file, 1000, 2, make_ip_frame(pcap::ip_proto_udp, 5, kPort, kPort, p2));
    append_record(file, 1000, 3, make_raw_frame(0x0806, /*filler=*/28));            // arp -> skip
    append_record(file, 1000, 4, make_ip_frame(/*tcp=*/6, 5, kPort, kPort, tcp_payload));  // skip
    append_record(file, 1000, 5, make_ip_frame(pcap::ip_proto_udp, 6, kPort, kPort, p3));  // ihl=6
    append_record(file, 1000, 6, make_ip_frame(pcap::ip_proto_udp, 5, kPort, kPort, p4));
    // a truncated trailing record: a full header that claims far more body than
    // exists. the cursor must stop here cleanly, having yielded all prior packets.
    put_u32_host(file, 1000);
    put_u32_host(file, 7);
    put_u32_host(file, 0xFFFFFFFFu);  // incl_len: impossibly large
    put_u32_host(file, 0xFFFFFFFFu);  // orig_len

    const std::string path = scratch_path();
    check(write_file(path, file));

    // --- map it back & replay ---
    PcapReader reader;
    check(reader.open(path.c_str()));
    check(reader.is_open());
    check_eq(reader.link_type(), pcap::linktype_ethernet);
    check(!reader.byte_swapped());   // we wrote host-order meta with the plain magic
    check(!reader.nanosecond());

    std::vector<market_event> events;
    std::size_t               packets = 0;
    udp_payload_view          view;
    PcapPacketCursor          cursor = reader.cursor();

    while (cursor.next(view)) {
        ++packets;
        check_eq(view.dst_port, kPort);  // every yielded datagram is our feed port
        check(view.len > 0);
        // hand the raw payload straight to the existing itch frame walker.
        itch::frame_cursor fc(view.data, view.len);
        std::uint16_t      blen = 0;
        for (const std::uint8_t* body = fc.next(blen); body != nullptr; body = fc.next(blen)) {
            market_event ev;
            if (itch::decode(body, blen, ev)) {
                events.push_back(ev);
            }
        }
    }

    // 4 udp packets survive (arp & tcp skipped); 6 itch messages inside them.
    check_eq(packets, 4u);
    check_eq(events.size(), 6u);

    // spot-check that fields survived the whole eth/ip/udp/itch round trip.
    check(events[0].type == event_type::add);
    check(events[0].s == side::bid);
    check_eq(events[0].order_id, 100u);
    check_eq(events[0].qty, 250u);
    check_eq(events[0].price, kBase + 10);

    check(events[1].type == event_type::add);   // p2, first frame
    check(events[1].s == side::ask);
    check_eq(events[1].price, kBase + 12);

    check(events[3].type == event_type::execute);  // p3, behind ipv4 options
    check_eq(events[3].order_id, 100u);
    check_eq(events[3].qty, 50u);

    check(events[4].type == event_type::delete_order);
    check_eq(events[4].order_id, 102u);

    check(events[5].type == event_type::trade);    // p4
    check(events[5].s == side::ask);
    check_eq(events[5].qty, 75u);
    check_eq(events[5].price, kBase + 12);

    // none of the skipped tcp payload leaked in (its ref 7777 must be absent).
    for (const market_event& ev : events) {
        check(ev.order_id != 7777u);
    }

    reader.close();
    check(!reader.is_open());
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// the engine path: replay straight into the order book, exactly as production
// would, & confirm a coherent book results.
// ---------------------------------------------------------------------------
void pcap_replay_feeds_the_book() {
    // a tiny but self-consistent two-sided book: two adds, one resting on each
    // side, then a partial execute against the bid.
    std::vector<std::uint8_t> payload;
    itch::encode_add(payload, kStock, 1, 1, side::bid, 500, kBase + 100);
    itch::encode_add(payload, kStock, 2, 2, side::ask, 400, kBase + 102);
    itch::encode_execute(payload, kStock, 3, 1, /*executed=*/200, /*match=*/1);

    std::vector<std::uint8_t> file;
    append_global_header(file);
    append_record(file, 1, 0, make_ip_frame(pcap::ip_proto_udp, 5, kPort, kPort, payload));

    const std::string path = scratch_path();
    check(write_file(path, file));

    PcapReader reader;
    check(reader.open(path.c_str()));

    order_book<8192, 1u << 16> book(kBase);
    feed_handler             handler;
    udp_payload_view         view;
    PcapPacketCursor         cursor = reader.cursor();
    std::uint64_t            applied = 0;
    std::uint64_t            rejected = 0;

    while (cursor.next(view)) {
        handler.process(view.data, view.len, [&](const market_event& ev) {
            if (apply_event(book, ev)) {
                ++applied;
            } else {
                ++rejected;
            }
        });
    }

    check_eq(handler.stats().messages, 3u);
    check_eq(handler.stats().malformed, 0u);
    check_eq(rejected, 0u);
    check_eq(applied, 3u);

    // both sides present; the bid was reduced 500 -> 300 by the execute.
    check(book.has_bid());
    check(book.has_ask());
    check_eq(book.best_bid(), kBase + 100);
    check_eq(book.best_ask(), kBase + 102);
    check_eq(book.best_bid_qty(), 300u);
    check_eq(book.best_ask_qty(), 400u);

    reader.close();
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// open() rejects bad inputs instead of mapping garbage.
// ---------------------------------------------------------------------------
void pcap_open_rejects_bad_files() {
    PcapReader reader;

    // a path that does not exist.
    check(!reader.open("/tmp/chronos_pcap_does_not_exist_xyz.pcap"));
    check(!reader.is_open());

    // a file too small to hold a 24-byte global header.
    const std::string short_path = scratch_path() + ".short";
    std::vector<std::uint8_t> tiny(8, 0x00);
    check(write_file(short_path, tiny));
    check(!reader.open(short_path.c_str()));
    std::remove(short_path.c_str());

    // a 24-byte header with an unrecognised magic.
    const std::string bad_path = scratch_path() + ".badmagic";
    std::vector<std::uint8_t> bad;
    put_u32_host(bad, 0xDEADBEEFu);  // not any known pcap magic
    while (bad.size() < pcap::global_header_size) {
        bad.push_back(0x00);
    }
    check(write_file(bad_path, bad));
    check(!reader.open(bad_path.c_str()));
    std::remove(bad_path.c_str());
}

}  // namespace

int main() {
    run_suite(pcap_replay_decodes_every_packet);
    run_suite(pcap_replay_feeds_the_book);
    run_suite(pcap_open_rejects_bad_files);
    return hft_test_summary("pcap");
}
