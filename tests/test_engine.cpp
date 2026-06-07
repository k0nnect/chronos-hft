// tests for phase 3: the fill model (latency, queue priority, partials, taker
// sweeps, fees), the metrics engine (average-cost p&l, drawdown, inventory), and
// a full engine run wiring book + fill model + strategy + metrics together.
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "check.hpp"
#include "hft/book/order_book.hpp"
#include "hft/engine/backtest_engine.hpp"
#include "hft/engine/fill_model.hpp"
#include "hft/engine/metrics.hpp"
#include "hft/engine/strategy.hpp"
#include "hft/feed/itch_generator.hpp"
#include "hft/feed/itch_parser.hpp"
#include "hft/feed/market_event.hpp"
#include "hft/strategies/micro_price_mm.hpp"

using namespace hft;

namespace {

using book_t = order_book<8192, 1u << 20>;
constexpr price_t kBase = 1'000'000;

bool approx(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// collect fills emitted by the model so a test can inspect them.
struct fill_collector {
    std::vector<fill_event> fills;
    void operator()(const fill_event& f) { fills.push_back(f); }
};

// build a simple two-sided book: bids 300@100 + asks 500@102, 400@103.
std::unique_ptr<book_t> make_book() {
    auto b = std::make_unique<book_t>(kBase);
    b->add(/*id=*/1, side::bid, kBase + 100, 300);
    b->add(/*id=*/2, side::ask, kBase + 102, 500);
    b->add(/*id=*/3, side::ask, kBase + 103, 400);
    return b;
}

// a passive bid only fills after the volume resting ahead of it has traded.
void queue_priority_passive_fill() {
    auto book = make_book();
    fill_model<64> fm(fill_config{/*latency=*/0, 0.0, 0.0, 1.0});
    fill_collector out;

    // post a passive bid at 100 (300 shares resting ahead of us) for 200 shares.
    fm.submit_limit(/*id=*/77, side::bid, kBase + 100, 200, /*now=*/0);
    // activate at t=1 (latency 0). no trade signal yet -> no fill, queue learned.
    fm.on_event(1, fill_signal{}, *book, out);
    check_eq(out.fills.size(), 0u);
    check_eq(fm.working_count(), 1u);

    // a 200-share trade at our price hits the queue ahead (300) -> still no fill.
    fm.on_event(2, fill_signal{true, true, side::bid, kBase + 100, 200}, *book, out);
    check_eq(out.fills.size(), 0u);

    // another 200-share trade: 100 left of the queue, then 100 reaches us.
    fm.on_event(3, fill_signal{true, true, side::bid, kBase + 100, 200}, *book, out);
    check_eq(out.fills.size(), 1u);
    check_eq(out.fills[0].qty, 100u);
    check(out.fills[0].maker);
    check_eq(out.fills[0].price, kBase + 100);

    // final 100-share trade fills the rest; order is now gone.
    fm.on_event(4, fill_signal{true, true, side::bid, kBase + 100, 100}, *book, out);
    check_eq(out.fills.size(), 2u);
    check_eq(out.fills[1].qty, 100u);
    check_eq(fm.working_count(), 0u);
}

// a cancel ahead of us advances our queue position but never fills us.
void cancel_advances_queue_without_filling() {
    auto book = make_book();
    fill_model<64> fm(fill_config{0, 0.0, 0.0, 1.0});
    fill_collector out;

    fm.submit_limit(/*id=*/5, side::bid, kBase + 100, 200, 0);
    fm.on_event(1, fill_signal{}, *book, out);  // activate, queue_ahead = 300

    // cancel 300 ahead of us (is_trade=false): queue clears, but no fill.
    fm.on_event(2, fill_signal{true, false, side::bid, kBase + 100, 300}, *book, out);
    check_eq(out.fills.size(), 0u);

    // now any trade at our price fills us immediately (we are at the front).
    fm.on_event(3, fill_signal{true, true, side::bid, kBase + 100, 120}, *book, out);
    check_eq(out.fills.size(), 1u);
    check_eq(out.fills[0].qty, 120u);
}

// latency: an order cannot fill before submit_time + latency.
void latency_gates_activation() {
    auto book = make_book();
    fill_model<64> fm(fill_config{/*latency=*/10, 0.0, 0.0, 1.0});
    fill_collector out;

    fm.submit_limit(/*id=*/9, side::bid, kBase + 100, 100, /*now=*/0);  // active at t=10
    // a trade at our price before activation must not fill (and we are at front,
    // queue learned only on activation).
    fm.on_event(5, fill_signal{true, true, side::bid, kBase + 100, 1000}, *book, out);
    check_eq(out.fills.size(), 0u);
    // at/after activation the queue is learned (300 ahead); a big trade fills us.
    fm.on_event(10, fill_signal{}, *book, out);            // activates, queue=300
    fm.on_event(11, fill_signal{true, true, side::bid, kBase + 100, 1000}, *book, out);
    check_eq(out.fills.size(), 1u);
    check_eq(out.fills[0].qty, 100u);
}

// a market order sweeps displayed liquidity across levels at activation, paying
// the taker fee, and drops any unfilled remainder (ioc).
void market_order_taker_sweep_and_fees() {
    auto book = make_book();  // asks: 500@102, 400@103
    fill_model<64> fm(fill_config{/*latency=*/0, /*maker=*/-0.20, /*taker=*/0.30, /*tv=*/1.0});
    fill_collector out;

    fm.submit_market(/*id=*/42, side::bid, /*qty=*/700, /*now=*/0);
    fm.on_event(1, fill_signal{}, *book, out);  // activates and sweeps now

    // 500 at 102 then 200 at 103.
    check_eq(out.fills.size(), 2u);
    check_eq(out.fills[0].price, kBase + 102);
    check_eq(out.fills[0].qty, 500u);
    check(!out.fills[0].maker);
    check_eq(out.fills[1].price, kBase + 103);
    check_eq(out.fills[1].qty, 200u);

    // taker fee = price*qty*tv*bps/1e4, positive (a cost).
    const double expect_fee0 = static_cast<double>(kBase + 102) * 500.0 * 0.30 / 10000.0;
    check(approx(out.fills[0].fee, expect_fee0));
    check(out.fills[0].fee > 0.0);
    check_eq(fm.working_count(), 0u);  // ioc: nothing left working
}

// average-cost p&l: buy 100@100 then sell 100@110 -> +1000 (minus fees=0 here).
void metrics_round_trip_pnl() {
    metrics_engine m(metrics_config{1.0});
    m.on_fill(fill_event{1, 1, kBase + 100, 100, side::bid, true, 0.0});
    check_eq(m.position(), 100);
    check(approx(m.avg_price(), static_cast<double>(kBase + 100)));

    m.on_fill(fill_event{2, 2, kBase + 110, 100, side::ask, true, 0.0});
    check_eq(m.position(), 0);
    check(approx(m.realized_pnl(), 1000.0));  // 10 ticks * 100 shares

    // mark with flat position -> unrealized 0, equity == realized.
    m.on_mark(static_cast<double>(kBase + 110));
    check(approx(m.unrealized_pnl(), 0.0));
    check(approx(m.equity(), 1000.0));
    check_eq(m.peak_inventory(), 100u);
    check_eq(m.fills(), 2u);
    check_eq(m.volume(), 200u);
}

// fees flow into realized p&l; a maker rebate (negative fee) increases it.
void metrics_fee_accounting() {
    metrics_engine m(metrics_config{1.0});
    // buy 100@100 with a maker rebate of -5 (credit), then flat at same price.
    m.on_fill(fill_event{1, 1, kBase + 100, 100, side::bid, true, -5.0});
    m.on_fill(fill_event{2, 2, kBase + 100, 100, side::ask, true, -5.0});
    check_eq(m.position(), 0);
    // realized = 0 price pnl - (sum of fees) = -(-10) = +10 rebate.
    check(approx(m.realized_pnl(), 10.0));
    check(approx(m.fees(), -10.0));
}

// short then cover: sell 50@105, buy 50@100 -> +250.
void metrics_short_then_cover() {
    metrics_engine m(metrics_config{1.0});
    m.on_fill(fill_event{1, 1, kBase + 105, 50, side::ask, true, 0.0});
    check_eq(m.position(), -50);
    m.on_fill(fill_event{2, 2, kBase + 100, 50, side::bid, true, 0.0});
    check_eq(m.position(), 0);
    check(approx(m.realized_pnl(), 250.0));  // (105-100)*50
}

// drawdown tracks the peak-to-trough of the equity curve.
void metrics_drawdown() {
    metrics_engine m(metrics_config{1.0});
    m.on_fill(fill_event{1, 1, kBase + 100, 100, side::bid, true, 0.0});  // long 100 @100
    m.on_mark(static_cast<double>(kBase + 110));  // equity +1000 (peak)
    check(approx(m.equity(), 1000.0));
    m.on_mark(static_cast<double>(kBase + 90));   // equity -1000
    check(approx(m.equity(), -1000.0));
    check(approx(m.max_drawdown(), 2000.0));      // 1000 peak -> -1000
    m.on_mark(static_cast<double>(kBase + 105));  // recovers to +500
    check(approx(m.max_drawdown(), 2000.0));      // peak drawdown is sticky
}

// full engine: replay a synthetic feed through book+fill+strategy+metrics and
// sanity-check that it runs, trades, and respects the position cap.
void full_engine_run_with_market_maker() {
    const itch::synthetic_feed feed = itch::generate_feed(100'000, /*seed=*/7);
    auto book = std::make_unique<book_t>(feed.base_tick);

    fill_model<256> fm(fill_config{/*latency=*/2, -0.20, 0.30, 1.0});
    metrics_engine  metrics(metrics_config{1.0});
    micro_price_mm  strat(micro_price_mm::config{/*quote_size=*/100, /*max_position=*/1000,
                                                 /*imb_lean=*/1.0, /*lean_threshold=*/0.6});
    backtest_engine<book_t, micro_price_mm, 256> engine(*book, strat, fm, metrics);

    market_event       ev;
    itch::frame_cursor cursor(feed.bytes.data(), feed.bytes.size());
    std::uint16_t      len = 0;
    std::uint64_t      processed = 0;
    for (const std::uint8_t* body = cursor.next(len); body; body = cursor.next(len)) {
        if (itch::decode(body, len, ev)) {
            engine.on_event(ev);
            ++processed;
        }
    }

    check_eq(processed, feed.messages);
    check_eq(engine.events_processed(), feed.messages);
    check(strat.requotes() > 0);          // it actually quoted
    check(metrics.fills() > 0);           // and got filled
    // hard inventory cap is respected (allow one in-flight quote of slack).
    check(std::llabs(metrics.position()) <= 1000 + 100);
    check(std::isfinite(metrics.equity()));
    check(std::isfinite(metrics.sharpe()));
}

}  // namespace

int main() {
    run_suite(queue_priority_passive_fill);
    run_suite(cancel_advances_queue_without_filling);
    run_suite(latency_gates_activation);
    run_suite(market_order_taker_sweep_and_fees);
    run_suite(metrics_round_trip_pnl);
    run_suite(metrics_fee_accounting);
    run_suite(metrics_short_then_cover);
    run_suite(metrics_drawdown);
    run_suite(full_engine_run_with_market_maker);
    return hft_test_summary("engine");
}
