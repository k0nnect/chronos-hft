// end-to-end backtest driver for the micro-price market maker.
//
// generates a synthetic itch stream, runs it through the phase-2 pipeline
// (parser -> spsc ring) on a producer thread, and on a consumer thread drains
// the ring into the backtest engine, which drives the book, fill model, strategy
// and metrics. prints the resulting trading statistics. this exercises every
// phase together with no virtual calls and no hot-path allocation.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "hft/book/order_book.hpp"
#include "hft/engine/backtest_engine.hpp"
#include "hft/engine/fill_model.hpp"
#include "hft/engine/metrics.hpp"
#include "hft/feed/feed_handler.hpp"
#include "hft/feed/itch_generator.hpp"
#include "hft/feed/spsc_ring.hpp"
#include "hft/strategies/micro_price_mm.hpp"

using namespace hft;

namespace {

// band covers base_tick .. base_tick + tick_band used by the generator.
using book_t = order_book<8192, 1u << 20>;
using ring_t = spsc_ring<market_event, 8192>;

}  // namespace

int main() {
    constexpr std::size_t kMessages = 2'000'000;

    const itch::synthetic_feed feed = itch::generate_feed(kMessages, /*seed=*/2024);

    auto book = std::make_unique<book_t>(feed.base_tick);
    auto ring = std::make_unique<ring_t>();

    // fill model: 5-tick-of-time tick-to-trade latency, maker rebate, taker fee.
    fill_model<256> fm(fill_config{/*latency_ns=*/5,
                                   /*maker_fee_bps=*/-0.20,
                                   /*taker_fee_bps=*/0.30,
                                   /*tick_value=*/1.0});
    metrics_engine metrics(metrics_config{/*tick_value=*/1.0});

    micro_price_mm strat(micro_price_mm::config{/*quote_size=*/100,
                                                /*max_position=*/2000,
                                                /*imb_lean=*/1.0,
                                                /*lean_threshold=*/0.6});

    backtest_engine<book_t, micro_price_mm, 256> engine(*book, strat, fm, metrics);

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    // producer: parse the wire stream into the ring (spinning if it fills).
    feed_handler handler;
    std::thread  producer([&] {
        ring_sink<ring_t> sink(*ring);
        handler.process(feed.bytes.data(), feed.bytes.size(), sink);
    });

    // consumer: drive the engine off the ring until every event is handled.
    std::thread consumer([&] {
        while (engine.events_processed() < feed.messages) {
            engine.drain(*ring);
        }
    });

    producer.join();
    consumer.join();
    const auto   t1 = clock::now();
    const double ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(kMessages);

    std::puts("== micro-price market maker backtest ==");
    std::printf("  events processed : %llu\n",
                static_cast<unsigned long long>(engine.events_processed()));
    std::printf("  wall time        : %.2f ns/event (full pipeline)\n", ns);
    std::printf("  requotes         : %llu\n",
                static_cast<unsigned long long>(strat.requotes()));
    std::printf("  strategy fills   : %llu (%llu shares)\n",
                static_cast<unsigned long long>(strat.fills()),
                static_cast<unsigned long long>(strat.filled_qty()));
    std::puts("  ---- pnl / risk ----");
    std::printf("  final position   : %lld shares\n",
                static_cast<long long>(metrics.position()));
    std::printf("  peak inventory   : %llu shares\n",
                static_cast<unsigned long long>(metrics.peak_inventory()));
    std::printf("  realized pnl     : %.2f\n", metrics.realized_pnl());
    std::printf("  unrealized pnl   : %.2f\n", metrics.unrealized_pnl());
    std::printf("  equity           : %.2f\n", metrics.equity());
    std::printf("  fees paid        : %.2f (negative = net rebate)\n", metrics.fees());
    std::printf("  max drawdown     : %.2f\n", metrics.max_drawdown());
    std::printf("  per-tick sharpe  : %.4f\n", metrics.sharpe());
    return 0;
}
