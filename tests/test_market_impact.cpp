// regression harness for the market-impact upgrade to the fill model.
//
// it locks down three things, each against the idealized (impact-off) baseline
// the same code path produces when `enable_impact` is false:
//   1. fixed-point primitives  -- the Q.16 sqrt/round/mul math is exact.
//   2. temporary impact        -- a taker sweep pays level-depletion-linear
//                                 slippage, so realized prices are strictly worse
//                                 & the marked p&l is strictly lower.
//   3. permanent impact + decay-- a large execution leaves a signed sqrt-law mid
//                                 skew that relaxes geometrically over later ticks
//                                 (measurable price decay).
//   4. queue degradation       -- our aggressive flow pushes our own resting
//                                 passive orders back in queue (adverse selection),
//                                 costing passive fills the baseline would capture.
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

#include "check.hpp"
#include "hft/book/order_book.hpp"
#include "hft/engine/fill_model.hpp"
#include "hft/engine/market_impact.hpp"
#include "hft/engine/metrics.hpp"

using namespace hft;

namespace {

using book_t = order_book<8192, 1u << 20>;
constexpr price_t kBase = 1'000'000;

bool approx(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// collect fills emitted by the model so a test can inspect them.
struct fill_collector {
    std::vector<fill_event> fills;
    void operator()(const fill_event& f) { fills.push_back(f); }
    [[nodiscard]] std::size_t maker_fills() const {
        std::size_t n = 0;
        for (const fill_event& f : fills) n += (f.maker ? 1u : 0u);
        return n;
    }
};

// bids: 300@100. asks: 500@102, 400@103. the standard fixture from the engine tests.
std::unique_ptr<book_t> make_book() {
    auto b = std::make_unique<book_t>(kBase);
    b->add(/*id=*/1, side::bid, kBase + 100, 300);
    b->add(/*id=*/2, side::ask, kBase + 102, 500);
    b->add(/*id=*/3, side::ask, kBase + 103, 400);
    return b;
}

// ---------------------------------------------------------------------------
// 1. fixed-point primitives are exact.
// ---------------------------------------------------------------------------
void fixed_point_math_is_exact() {
    using namespace impact_detail;

    // sqrt(4) == 2, sqrt(9) == 3, sqrt(2.25) == 1.5 in Q.16.
    check_eq(fp_sqrt(fp_from_int(4)), fp_from_int(2));
    check_eq(fp_sqrt(fp_from_int(9)), fp_from_int(3));
    check(approx(fp_to_double(fp_sqrt(fp_from_double(2.25))), 1.5));

    // round-to-nearest, symmetric about zero.
    check_eq(fp_round(fp_from_double(2.5)), 3);
    check_eq(fp_round(fp_from_double(2.4)), 2);
    check_eq(fp_round(fp_from_double(-2.5)), -3);

    // mul / div round-trip on representable values.
    check(approx(fp_to_double(fp_mul(fp_from_double(4.0), fp_from_double(0.5))), 2.0));
    check(approx(fp_to_double(fp_div(fp_from_int(7), fp_from_int(2))), 3.5));
    check_eq(fp_div(fp_from_int(1), 0), 0);  // guarded divide-by-zero

    // the impact module's temporary skew is linear in depletion: half a level
    // costs half the coefficient.
    market_impact mi(impact_config{/*temp=*/4.0, 0.0, 1.0, 0.05, 0.0, 1, 0});
    check_eq(fp_round(mi.temporary_skew(/*filled=*/500, /*avail=*/500)), 4);
    check_eq(fp_round(mi.temporary_skew(/*filled=*/250, /*avail=*/500)), 2);
    check_eq(fp_round(mi.temporary_skew(/*filled=*/0, /*avail=*/0)), 0);
}

// ---------------------------------------------------------------------------
// 2. temporary impact: a taker sweep pays worse prices & shows lower p&l than
//    the identical sweep with impact off.
// ---------------------------------------------------------------------------
void temporary_impact_worsens_taker_and_pnl() {
    auto book = make_book();

    // baseline: idealized liquidity (impact off).
    fill_model<64> base(fill_config{/*latency=*/0, 0.0, 0.0, /*tv=*/1.0});
    fill_collector base_out;
    base.submit_market(/*id=*/42, side::bid, /*qty=*/700, /*now=*/0);
    base.on_event(1, fill_signal{}, *book, base_out);

    check_eq(base_out.fills.size(), 2u);
    check_eq(base_out.fills[0].price, kBase + 102);  // 500 @ displayed 102
    check_eq(base_out.fills[0].qty, 500u);
    check_eq(base_out.fills[1].price, kBase + 103);  // 200 @ displayed 103
    check_eq(base_out.fills[1].qty, 200u);

    // impact on: temporary slippage of 4 ticks at full-level depletion.
    fill_config icfg{/*latency=*/0, 0.0, 0.0, /*tv=*/1.0};
    icfg.enable_impact         = true;
    icfg.impact.temp_coeff     = 4.0;
    icfg.impact.default_volume = 700;  // pin the rolling-volume denominator
    fill_model<64> imp(icfg);
    fill_collector imp_out;
    imp.submit_market(/*id=*/43, side::bid, /*qty=*/700, /*now=*/0);
    imp.on_event(1, fill_signal{}, *book, imp_out);

    check_eq(imp_out.fills.size(), 2u);
    // level 1 fully depleted (500/500 -> +4): 102 + 4 = 106.
    check_eq(imp_out.fills[0].price, kBase + 106);
    check_eq(imp_out.fills[0].qty, 500u);
    // level 2 half depleted (200/400 -> +2): 103 + 2 = 105.
    check_eq(imp_out.fills[1].price, kBase + 105);
    check_eq(imp_out.fills[1].qty, 200u);

    // every impacted child fill is at least as expensive, & the sweep total is
    // strictly worse.
    check(imp_out.fills[0].price > base_out.fills[0].price);
    check(imp_out.fills[1].price > base_out.fills[1].price);

    // fold each into metrics & mark both at the same reference mid: the impacted
    // run carries a higher average cost, hence strictly lower marked equity.
    const double mark = static_cast<double>(kBase) + 102.5;
    metrics_engine m_base(metrics_config{1.0});
    metrics_engine m_imp(metrics_config{1.0});
    for (const fill_event& f : base_out.fills) m_base.on_fill(f);
    for (const fill_event& f : imp_out.fills) m_imp.on_fill(f);
    m_base.on_mark(mark);
    m_imp.on_mark(mark);

    check_eq(m_base.position(), 700);
    check_eq(m_imp.position(), 700);
    check(m_imp.avg_price() > m_base.avg_price());
    check(m_imp.equity() < m_base.equity());   // impact strictly lowers realized edge
    check(std::isfinite(m_imp.equity()));
}

// ---------------------------------------------------------------------------
// 3. permanent impact: a buy lifts the mid by a sqrt-law amount that then decays
//    geometrically; a sell pushes it the other way.
// ---------------------------------------------------------------------------
void permanent_impact_skews_then_decays() {
    auto book = make_book();

    fill_config icfg{/*latency=*/0, 0.0, 0.0, /*tv=*/1.0};
    icfg.enable_impact         = true;
    icfg.impact.perm_coeff     = 8.0;   // ticks at unit participation
    icfg.impact.decay          = 0.5;   // halve the skew each event
    icfg.impact.default_volume = 700;   // participation = 700/700 = 1 -> sqrt = 1
    fill_model<64> imp(icfg);
    fill_collector out;

    check(approx(imp.mid_skew_ticks(), 0.0));  // nothing accumulated yet

    // a 700-share market buy: skew = perm_coeff * sqrt(700/700) = +8.0 ticks.
    imp.submit_market(/*id=*/7, side::bid, /*qty=*/700, /*now=*/0);
    imp.on_event(1, fill_signal{}, *book, out);
    check(approx(imp.mid_skew_ticks(), 8.0));
    check(imp.impacted_mid(1000.0) > 1000.0);  // mid displaced upward

    // each subsequent (quiet) event relaxes the skew by the decay factor.
    double prev = imp.mid_skew_ticks();
    for (int i = 0; i < 4; ++i) {
        imp.on_event(static_cast<ts_t>(2 + i), fill_signal{}, *book, out);
        const double now_skew = imp.mid_skew_ticks();
        check(now_skew < prev);       // strictly decaying
        check(now_skew > 0.0);        // same sign, not overshooting zero
        prev = now_skew;
    }
    check(approx(imp.mid_skew_ticks(), 8.0 * 0.5 * 0.5 * 0.5 * 0.5));  // four post-sweep decays

    // symmetry: an aggressive sell depresses the mid (negative skew).
    fill_config scfg{/*latency=*/0, 0.0, 0.0, 1.0};
    scfg.enable_impact         = true;
    scfg.impact.perm_coeff     = 8.0;
    scfg.impact.default_volume = 300;
    fill_model<64> sell(scfg);
    fill_collector sell_out;
    sell.submit_market(/*id=*/8, side::ask, /*qty=*/300, /*now=*/0);  // sweeps the 300@100 bid
    sell.on_event(1, fill_signal{}, *book, sell_out);
    check(sell.mid_skew_ticks() < 0.0);
    check(approx(sell.mid_skew_ticks(), -8.0));
}

// ---------------------------------------------------------------------------
// 4. queue degradation: our taker flow inflates the queue ahead of our own
//    resting passive order, so a trade that fills the baseline misses with impact.
// ---------------------------------------------------------------------------
void queue_degradation_costs_passive_fills() {
    // --- baseline: no impact, the passive order keeps its measured queue. ---
    {
        auto book = make_book();
        fill_model<64> base(fill_config{/*latency=*/0, 0.0, 0.0, 1.0});
        fill_collector out;

        base.submit_limit(/*id=*/1, side::bid, kBase + 100, /*qty=*/100, /*now=*/0);
        base.on_event(1, fill_signal{}, *book, out);          // activate: queue_ahead = 300
        base.submit_market(/*id=*/2, side::bid, /*qty=*/700, /*now=*/1);
        base.on_event(2, fill_signal{}, *book, out);          // taker sweep (no degradation)

        // a 350-share trade at our price: 300 ahead consumed, 50 reaches us.
        base.on_event(3, fill_signal{true, true, side::bid, kBase + 100, 350}, *book, out);
        check_eq(out.maker_fills(), 1u);
        // find the maker fill & check its size.
        qty_t maker_qty = 0;
        for (const fill_event& f : out.fills) {
            if (f.maker) maker_qty = f.qty;
        }
        check_eq(maker_qty, 50u);
    }

    // --- impact on: the same flow pushes our queue back by 80 shares. ---
    {
        auto book = make_book();
        fill_config icfg{/*latency=*/0, 0.0, 0.0, 1.0};
        icfg.enable_impact         = true;
        icfg.impact.queue_coeff    = 80.0;  // 80 * sqrt(700/700) = 80 shares of push-back
        icfg.impact.default_volume = 700;
        fill_model<64> imp(icfg);
        fill_collector out;

        imp.submit_limit(/*id=*/1, side::bid, kBase + 100, /*qty=*/100, /*now=*/0);
        imp.on_event(1, fill_signal{}, *book, out);           // activate: queue_ahead = 300
        imp.submit_market(/*id=*/2, side::bid, /*qty=*/700, /*now=*/1);
        imp.on_event(2, fill_signal{}, *book, out);           // taker sweep -> queue_ahead = 380

        const std::size_t before = out.maker_fills();
        // the same 350-share trade now falls short of the degraded 380 queue: no fill.
        imp.on_event(3, fill_signal{true, true, side::bid, kBase + 100, 350}, *book, out);
        check_eq(out.maker_fills(), before);  // no new maker fill -- passive edge lost
        check_eq(out.maker_fills(), 0u);
    }
}

}  // namespace

int main() {
    run_suite(fixed_point_math_is_exact);
    run_suite(temporary_impact_worsens_taker_and_pnl);
    run_suite(permanent_impact_skews_then_decays);
    run_suite(queue_degradation_costs_passive_fills);
    return hft_test_summary("market_impact");
}
