// end-to-end driver for the statistical market maker, fed from an offline pcap
// multicast capture.
//
//   pcap file --mmap--> PcapReader --udp payloads--> feed_handler --itch events-->
//   backtest_engine( order_book + fill_model + statistical_mm + metrics )
//
// give a capture path on the command line to replay it; with no argument the
// driver synthesizes a deterministic itch feed, wraps it in ethernet/ipv4/udp &
// writes a temporary .pcap so the full pcap path is always exercised. it streams
// a jsonl trace (`stat_trace.jsonl`) carrying the book, p&l & the strategy's alpha
// / inventory state for the replay dashboard.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hft/book/order_book.hpp"
#include "hft/core/byte_order.hpp"
#include "hft/engine/backtest_engine.hpp"
#include "hft/engine/fill_model.hpp"
#include "hft/engine/metrics.hpp"
#include "hft/feed/feed_handler.hpp"
#include "hft/feed/itch_generator.hpp"
#include "hft/feed/pcap_reader.hpp"
#include "hft/feed/pcap_structures.hpp"
#include "hft/metrics/trace_logger.hpp"
#include "hft/strategies/statistical_mm.hpp"

using namespace hft;

namespace {

using book_t = order_book<1u << 16, 1u << 20>;
constexpr price_t kBase = 1'000'000;  // matches the synthetic generator's band

// ---- minimal pcap savefile writer (cold setup; tests/driver may allocate) ----

void put_u32_host(std::vector<std::uint8_t>& b, std::uint32_t v) {
    std::uint8_t t[4];
    std::memcpy(t, &v, sizeof(t));  // host order: pairs with the plain pcap magic
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

void append_global_header(std::vector<std::uint8_t>& file) {
    put_u32_host(file, pcap::magic_micros);
    put_be<std::uint16_t>(file, 2);  // version major
    put_be<std::uint16_t>(file, 4);  // version minor
    put_u32_host(file, 0);                        // thiszone
    put_u32_host(file, 0);                        // sigfigs
    put_u32_host(file, 65535);                    // snaplen
    put_u32_host(file, pcap::linktype_ethernet);  // network
}

// wrap a span of itch bytes as ethernet/ipv4/udp & return the captured frame.
std::vector<std::uint8_t> make_udp_frame(const std::uint8_t* payload, std::size_t payload_len,
                                         std::uint16_t port) {
    std::vector<std::uint8_t> frame;
    const std::uint8_t dst[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};  // ip multicast mac
    const std::uint8_t src[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    put_bytes(frame, dst, sizeof(dst));
    put_bytes(frame, src, sizeof(src));
    put_be<std::uint16_t>(frame, pcap::ethertype_ipv4);

    const std::size_t ip_total = pcap::ipv4_min_size + pcap::udp_size + payload_len;
    frame.push_back(0x45);                                                 // version 4, ihl 5
    frame.push_back(0x00);                                                 // dscp/ecn
    put_be<std::uint16_t>(frame, static_cast<std::uint16_t>(ip_total));    // total length
    put_be<std::uint16_t>(frame, 0x0000);                                  // identification
    put_be<std::uint16_t>(frame, 0x4000);                                  // flags: don't fragment
    frame.push_back(64);                                                   // ttl
    frame.push_back(pcap::ip_proto_udp);                                   // protocol
    put_be<std::uint16_t>(frame, 0x0000);                                  // header checksum
    put_be<std::uint32_t>(frame, 0xC0A80001u);                            // src 192.168.0.1
    put_be<std::uint32_t>(frame, 0xEF000001u);                            // dst 239.0.0.1

    put_be<std::uint16_t>(frame, port);                                    // udp src port
    put_be<std::uint16_t>(frame, port);                                    // udp dst port
    put_be<std::uint16_t>(frame, static_cast<std::uint16_t>(pcap::udp_size + payload_len));
    put_be<std::uint16_t>(frame, 0x0000);                                  // udp checksum
    put_bytes(frame, payload, payload_len);
    return frame;
}

void append_record(std::vector<std::uint8_t>& file, std::uint32_t ts_sec,
                   const std::vector<std::uint8_t>& frame) {
    put_u32_host(file, ts_sec);
    put_u32_host(file, 0);  // ts fractional
    put_u32_host(file, static_cast<std::uint32_t>(frame.size()));  // incl_len
    put_u32_host(file, static_cast<std::uint32_t>(frame.size()));  // orig_len
    put_bytes(file, frame.data(), frame.size());
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

// synthesize a deterministic capture: generate an itch stream, then pack whole
// length-prefixed frames into ~mtu-sized udp datagrams, one record each.
bool build_synthetic_pcap(const std::string& path, std::size_t messages) {
    const itch::synthetic_feed feed = itch::generate_feed(messages, /*seed=*/4242);
    const std::uint8_t* const  data = feed.bytes.data();
    const std::size_t          total = feed.bytes.size();

    std::vector<std::uint8_t> file;
    file.reserve(total + total / 16 + 1024);
    append_global_header(file);

    constexpr std::size_t kMaxPayload = 1400;  // keep datagrams below a typical mtu
    constexpr std::uint16_t kPort     = 30001;
    std::size_t   pos = 0;
    std::uint32_t ts  = 0;
    while (pos < total) {
        const std::size_t start = pos;
        std::size_t       plen  = 0;
        // accumulate whole [len][body] frames until the next would overflow the mtu.
        while (pos < total) {
            if (pos + itch::frame_header_size > total) {
                pos = total;  // truncated tail (should not happen for our generator)
                break;
            }
            const std::uint16_t flen = load_be<std::uint16_t>(data + pos);
            const std::size_t   fsz  = itch::frame_header_size + static_cast<std::size_t>(flen);
            if (pos + fsz > total) {
                pos = total;
                break;
            }
            if (plen > 0 && plen + fsz > kMaxPayload) {
                break;
            }
            plen += fsz;
            pos += fsz;
        }
        if (plen == 0) {
            break;
        }
        append_record(file, ts++, make_udp_frame(data + start, plen, kPort));
    }
    return write_file(path, file);
}

}  // namespace

int main(int argc, char** argv) {
    // 1. obtain a pcap path: user-provided, or a freshly synthesized capture.
    std::string path;
    std::string synth_path;
    if (argc > 1) {
        path = argv[1];
    } else {
        synth_path = "stat_mm_synth.pcap";
        if (!build_synthetic_pcap(synth_path, /*messages=*/400'000)) {
            std::fprintf(stderr, "failed to synthesize %s\n", synth_path.c_str());
            return 1;
        }
        path = synth_path;
    }

    PcapReader reader;
    if (!reader.open(path.c_str())) {
        std::fprintf(stderr, "could not open pcap: %s\n", path.c_str());
        return 1;
    }

    // 2. wire the engine: book + fill model + statistical maker + metrics.
    auto           book = std::make_unique<book_t>(kBase);
    fill_model<256> fm(fill_config{/*latency_ns=*/5, /*maker=*/-0.20, /*taker=*/0.30, /*tv=*/1.0});
    metrics_engine  metrics(metrics_config{/*tick_value=*/1.0});
    statistical_mm  strat(statistical_mm::default_config());
    backtest_engine<book_t, statistical_mm, 256> engine(*book, strat, fm, metrics);

    // 3. trace: capture book, p&l & the strategy's alpha / inventory state.
    trace_logger<>    tracer;
    const char* const trace_path = "stat_trace.jsonl";
    const bool        tracing     = tracer.open(trace_path);
    if (tracing) {
        tracer.set_interval(3000);             // synthetic stamps are message-index ns
        tracer.attach_extras(&strat.trace_state());
        engine.attach_trace_logger(&tracer);
    }

    // 4. replay: peel udp payloads zero-copy & drive each itch event through.
    feed_handler     handler;
    PcapPacketCursor cursor = reader.cursor();
    udp_payload_view view;
    std::uint64_t    packets = 0;
    while (cursor.next(view)) {
        ++packets;
        handler.process(view.data, view.len,
                        [&](const market_event& ev) noexcept { engine.on_event(ev); });
    }

    // 5. report.
    std::puts("== statistical market maker (pcap replay) ==");
    std::printf("  source pcap      : %s\n", path.c_str());
    std::printf("  udp packets      : %llu\n", static_cast<unsigned long long>(packets));
    std::printf("  itch messages    : %llu\n",
                static_cast<unsigned long long>(handler.stats().messages));
    std::printf("  events processed : %llu\n",
                static_cast<unsigned long long>(engine.events_processed()));
    std::printf("  requotes         : %llu\n",
                static_cast<unsigned long long>(strat.requotes()));
    std::printf("  strategy fills   : %llu (%llu shares)\n",
                static_cast<unsigned long long>(strat.fills()),
                static_cast<unsigned long long>(strat.filled_qty()));
    std::puts("  ---- alpha state (final) ----");
    std::printf("  raw obi          : %+.4f\n", strat.alpha_engine().obi());
    std::printf("  filtered alpha   : %+.4f\n", strat.alpha_engine().alpha());
    std::printf("  alpha velocity   : %+.6f\n", strat.alpha_engine().velocity());
    std::puts("  ---- pnl / risk ----");
    std::printf("  final position   : %lld shares\n",
                static_cast<long long>(metrics.position()));
    std::printf("  peak inventory   : %llu shares\n",
                static_cast<unsigned long long>(metrics.peak_inventory()));
    std::printf("  realized pnl     : %.2f\n", metrics.realized_pnl());
    std::printf("  equity           : %.2f\n", metrics.equity());
    std::printf("  fees paid        : %.2f (negative = net rebate)\n", metrics.fees());
    std::printf("  max drawdown     : %.2f\n", metrics.max_drawdown());
    std::printf("  per-tick sharpe  : %.4f\n", metrics.sharpe());

    if (tracing) {
        const std::uint64_t frames = tracer.lines();
        tracer.close();  // flush
        std::puts("  ---- replay trace ----");
        std::printf("  wrote %s: %llu frames (load it in the dashboard)\n", trace_path,
                    static_cast<unsigned long long>(frames));
    }

    reader.close();
    if (!synth_path.empty()) {
        std::remove(synth_path.c_str());  // tidy up the temporary capture
    }
    return 0;
}
