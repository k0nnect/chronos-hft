// phase 2 micro-benchmarks: raw decode throughput, the spsc ring across two
// threads, & the full feed -> ring -> book pipeline end to end.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "hft/book/apply.hpp"
#include "hft/book/order_book.hpp"
#include "hft/feed/feed_handler.hpp"
#include "hft/feed/itch_generator.hpp"
#include "hft/feed/spsc_ring.hpp"

using namespace hft;

namespace {

using steady   = std::chrono::steady_clock;
using book_t   = order_book<8192, 1u << 20>;
using ring_t   = spsc_ring<market_event, 8192>;

double ns_per(std::chrono::nanoseconds d, std::uint64_t n) {
    return static_cast<double>(d.count()) / static_cast<double>(n);
}

void bench_decode() {
    std::puts("== decode throughput (single thread) ==");
    const itch::synthetic_feed feed = itch::generate_feed(2'000'000, /*seed=*/7);

    feed_handler  handler;
    std::uint64_t checksum = 0;  // keep the optimiser honest
    const auto    t0       = steady::now();
    handler.process(feed.bytes.data(), feed.bytes.size(),
                    [&](const market_event& ev) { checksum += ev.order_id + ev.qty; });
    const auto t1 = steady::now();

    const auto   dt   = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
    const double mbps = static_cast<double>(feed.bytes.size()) / 1e6 /
                        (static_cast<double>(dt.count()) / 1e9);
    std::printf("  %llu msgs, %zu bytes\n",
                static_cast<unsigned long long>(handler.stats().messages), feed.bytes.size());
    std::printf("  %.2f ns/msg | %.0f msgs/s | %.0f MB/s | checksum=%llu\n",
                ns_per(dt, handler.stats().messages),
                static_cast<double>(handler.stats().messages) /
                    (static_cast<double>(dt.count()) / 1e9),
                mbps, static_cast<unsigned long long>(checksum));
}

void bench_ring() {
    std::puts("\n== spsc ring throughput (2 threads) ==");
    constexpr std::uint64_t kCount = 50'000'000;
    auto ring = std::make_unique<spsc_ring<std::uint64_t, 8192>>();

    const auto t0 = steady::now();
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!ring->try_push(i)) { /* spin */ }
        }
    });
    std::uint64_t sink = 0;
    std::thread consumer([&] {
        std::uint64_t out  = 0;
        std::uint64_t seen = 0;
        while (seen < kCount) {
            if (ring->try_pop(out)) {
                sink += out;
                ++seen;
            }
        }
    });
    producer.join();
    consumer.join();
    const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(steady::now() - t0);
    std::printf("  %llu items | %.2f ns/item | %.0f Mitems/s | sink=%llu\n",
                static_cast<unsigned long long>(kCount), ns_per(dt, kCount),
                static_cast<double>(kCount) / (static_cast<double>(dt.count()) / 1e9) / 1e6,
                static_cast<unsigned long long>(sink));
}

void bench_pipeline() {
    std::puts("\n== full pipeline: feed -> ring -> book (2 threads) ==");
    const itch::synthetic_feed feed = itch::generate_feed(2'000'000, /*seed=*/21);
    auto book = std::make_unique<book_t>(feed.base_tick);
    auto ring = std::make_unique<ring_t>();

    feed_handler handler;
    const auto   t0 = steady::now();
    std::thread  producer([&] {
        ring_sink<ring_t> sink(*ring);
        handler.process(feed.bytes.data(), feed.bytes.size(), sink);
    });
    std::thread consumer([&] {
        market_event  ev;
        std::uint64_t seen = 0;
        while (seen < feed.messages) {
            if (ring->try_pop(ev)) {
                (void)apply_event(*book, ev);
                ++seen;
            }
        }
    });
    producer.join();
    consumer.join();
    const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(steady::now() - t0);
    std::printf("  %llu events | %.2f ns/event | %.0f Mevents/s\n",
                static_cast<unsigned long long>(feed.messages), ns_per(dt, feed.messages),
                static_cast<double>(feed.messages) / (static_cast<double>(dt.count()) / 1e9) / 1e6);
    std::printf("  resting orders after replay: %zu (expected %zu)\n", book->live_orders(),
                feed.expected_live);
}

}  // namespace

int main() {
    bench_decode();
    bench_ring();
    bench_pipeline();
    return 0;
}
