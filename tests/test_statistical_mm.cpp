// tests for the statistical market maker: that it quotes a sane two-sided book,
// respects the hard inventory cap, & skews its quote center in the direction of
// the multi-level alpha signal.
#include <cstddef>

#include "check.hpp"
#include "hft/engine/book_update.hpp"
#include "hft/engine/fill_model.hpp"
#include "hft/engine/strategy.hpp"
#include "hft/strategies/statistical_mm.hpp"

using namespace hft;

namespace {

constexpr price_t kBase = 1'000'000;

// build a two-sided snapshot. depth sizes are supplied per side (5 levels), the
// prices ladder one tick out from the touch.
book_update make_update(price_t bb, price_t ba, qty_t bq, qty_t aq, const qty_t (&bsz)[5],
                        const qty_t (&asz)[5]) {
    book_update u;
    u.two_sided  = true;
    u.best_bid   = bb;
    u.best_ask   = ba;
    u.bid_qty    = bq;
    u.ask_qty    = aq;
    u.mid        = 0.5 * static_cast<double>(bb + ba);
    u.micro_price = u.mid;
    u.bid_levels = 5;
    u.ask_levels = 5;
    for (std::size_t k = 0; k < 5; ++k) {
        u.bid_px[k] = bb - static_cast<price_t>(k);
        u.ask_px[k] = ba + static_cast<price_t>(k);
        u.bid_sz[k] = bsz[k];
        u.ask_sz[k] = asz[k];
    }
    return u;
}

// pull the resting bid / ask quote prices out of the gateway's staged intents.
struct quotes {
    bool    has_bid = false;
    bool    has_ask = false;
    price_t bid_px  = 0;
    price_t ask_px  = 0;
};

quotes collect(const order_gateway& gw) {
    quotes q;
    for (std::size_t i = 0; i < gw.count(); ++i) {
        const order_intent& it = gw[i];
        if (it.action != order_action::limit) continue;
        if (it.s == side::bid) {
            q.has_bid = true;
            q.bid_px  = it.price;
        } else {
            q.has_ask = true;
            q.ask_px  = it.price;
        }
    }
    return q;
}

// a flat, balanced book: quotes both sides, bid below ask, inside the touch.
void quotes_a_sane_two_sided_market() {
    order_gateway gw;
    statistical_mm strat(statistical_mm::default_config());
    strat.bind(gw);

    const qty_t bsz[5] = {500, 500, 500, 500, 500};
    const qty_t asz[5] = {500, 500, 500, 500, 500};
    const book_update u = make_update(kBase + 100, kBase + 104, 500, 500, bsz, asz);

    strat.on_book_update(u);
    const quotes q = collect(gw);

    check(q.has_bid);
    check(q.has_ask);
    check(q.bid_px < q.ask_px);          // a real, uncrossed quote
    check(q.bid_px <= u.best_ask - 1);   // never crosses the resting ask
    check(q.ask_px >= u.best_bid + 1);   // never crosses the resting bid
    check(strat.requotes() == 1u);
}

// once inventory hits the cap the breached side stops quoting.
void respects_inventory_cap() {
    const qty_t bal[5] = {400, 400, 400, 400, 400};
    const book_update u = make_update(kBase + 100, kBase + 104, 400, 400, bal, bal);

    // long at the cap: no more bidding.
    {
        order_gateway gw;
        statistical_mm strat(statistical_mm::default_config());  // max_position 2000
        strat.bind(gw);
        strat.on_order_fill(fill_event{1, 1, kBase + 100, 2000, side::bid, true, 0.0});
        check_eq(strat.net_position(), 2000);
        strat.on_book_update(u);
        const quotes q = collect(gw);
        check(!q.has_bid);  // at the long cap: stop bidding
        check(q.has_ask);   // still willing to sell down inventory
    }

    // short at the cap: no more offering.
    {
        order_gateway gw;
        statistical_mm strat(statistical_mm::default_config());
        strat.bind(gw);
        strat.on_order_fill(fill_event{1, 2, kBase + 104, 2000, side::ask, true, 0.0});
        check_eq(strat.net_position(), -2000);
        strat.on_book_update(u);
        const quotes q = collect(gw);
        check(q.has_bid);   // still willing to buy back
        check(!q.has_ask);  // at the short cap: stop offering
    }
}

// with the top of book held equal, a deep bid-heavy book skews quotes higher than
// a deep ask-heavy book -- the alpha signal moving the center.
void alpha_skews_the_quote_center() {
    // identical touch (500x500) so the micro-price is the same; only deep size
    // differs, so any center shift is the multi-level alpha, not the micro-price.
    const qty_t heavy[5] = {500, 900, 900, 900, 900};
    const qty_t light[5] = {500, 100, 100, 100, 100};

    order_gateway gw_bull;
    statistical_mm bull(statistical_mm::default_config());
    bull.bind(gw_bull);
    bull.on_book_update(make_update(kBase + 100, kBase + 104, 500, 500, heavy, light));
    const quotes qb = collect(gw_bull);

    order_gateway gw_bear;
    statistical_mm bear(statistical_mm::default_config());
    bear.bind(gw_bear);
    bear.on_book_update(make_update(kBase + 100, kBase + 104, 500, 500, light, heavy));
    const quotes qr = collect(gw_bear);

    check(qb.has_bid && qb.has_ask && qr.has_bid && qr.has_ask);
    // the bid-heavy center sits strictly above the ask-heavy center.
    check(qb.bid_px + qb.ask_px > qr.bid_px + qr.ask_px);

    // & the trace state reflects a positive vs negative alpha.
    check(bull.trace_state().alpha > 0.0);
    check(bear.trace_state().alpha < 0.0);
    check(bull.trace_state().present);
}

}  // namespace

int main() {
    run_suite(quotes_a_sane_two_sided_market);
    run_suite(respects_inventory_cap);
    run_suite(alpha_skews_the_quote_center);
    return hft_test_summary("statistical_mm");
}
