// the backtest engine: the single hot loop that turns market events into book
// mutations, simulated fills, strategy decisions and metrics.
//
// per event the sequence is deliberate and models real causality:
//   1. resolve the consumption signal from the *pre-mutation* book (an execute /
//      cancel / delete needs the resting order's price+side before it is gone).
//   2. apply the event to the book.
//   3. tick the fill model against the new book; any fills are delivered to the
//      strategy (on_order_fill) and folded into metrics.
//   4. hand the strategy the raw event and a fresh top-of-book snapshot; it may
//      stage new orders / cancels via the gateway.
//   5. mark metrics to the new fair value.
//   6. drain the staged intents into the fill model (stamped with this event's
//      time, so their tick-to-trade latency starts now).
//
// templated on the concrete book and strategy types: no virtual dispatch anywhere
// on this path. nothing here allocates or throws.
#pragma once

#include <cstddef>

#include "hft/book/apply.hpp"
#include "hft/core/compiler.hpp"
#include "hft/engine/book_update.hpp"
#include "hft/engine/fill_model.hpp"
#include "hft/engine/metrics.hpp"
#include "hft/engine/strategy.hpp"
#include "hft/feed/market_event.hpp"

namespace hft {

template <typename Book, typename Strategy, std::size_t MaxWorking = 256>
class backtest_engine {
public:
    using fill_model_t = fill_model<MaxWorking>;

    backtest_engine(Book& book, Strategy& strat, fill_model_t& fm,
                    metrics_engine& metrics) noexcept
        : book_(&book), strat_(&strat), fm_(&fm), metrics_(&metrics) {
        strat_->bind(gw_);
    }

    backtest_engine(const backtest_engine&)            = delete;
    backtest_engine& operator=(const backtest_engine&) = delete;

    // process exactly one market event end to end.
    hft_hot void on_event(const market_event& ev) noexcept {
        const ts_t        now = ev.timestamp;
        const fill_signal sig = resolve_signal(ev);  // before the book changes

        apply_event(*book_, ev);

        // fills produced by this event reach the strategy and metrics first.
        fm_->on_event(now, sig, *book_,
                      [this](const fill_event& f) noexcept { deliver_fill(f); });

        // strategy reacts to the raw event and to fresh top of book.
        strat_->handle_market_event(ev);
        if (book_->has_bid() && book_->has_ask()) [[likely]] {
            const book_update u = snapshot(now);
            strat_->handle_book_update(u);
            if (u.micro_price == u.micro_price) {  // finite (not nan)
                metrics_->on_mark(u.micro_price);
            }
        }

        // newly staged orders become live from this timestamp onward.
        drain_intents(now);
        ++events_;
    }

    // pull and process every event currently available in an spsc ring; returns
    // how many were handled. call repeatedly while the producer is still running.
    template <typename Ring>
    hft_hot std::size_t drain(Ring& ring) noexcept {
        market_event ev;
        std::size_t  count = 0;
        while (ring.try_pop(ev)) {
            on_event(ev);
            ++count;
        }
        return count;
    }

    [[nodiscard]] std::uint64_t events_processed() const noexcept { return events_; }
    [[nodiscard]] const metrics_engine& metrics() const noexcept { return *metrics_; }
    [[nodiscard]] const fill_model_t& fills() const noexcept { return *fm_; }

private:
    hft_always_inline void deliver_fill(const fill_event& f) noexcept {
        strat_->handle_order_fill(f);
        metrics_->on_fill(f);
    }

    // resolve the queue-consumption signal a market event represents. trades
    // carry price/side directly; executes/cancels/deletes/replaces are looked up
    // in the still-intact book. an `is_trade` signal can fill our resting orders;
    // a cancel/delete signal only advances our queue position.
    [[nodiscard]] hft_hot fill_signal resolve_signal(const market_event& ev) const noexcept {
        fill_signal sig;
        side        s;
        price_t     px;
        qty_t       q;
        switch (ev.type) {
            case event_type::trade:
                sig = fill_signal{true, true, ev.s, ev.price, ev.qty};
                break;
            case event_type::execute:
                if (book_->peek_order(ev.order_id, s, px, q)) {
                    sig = fill_signal{true, true, s, px, ev.qty};
                }
                break;
            case event_type::reduce:
                if (book_->peek_order(ev.order_id, s, px, q)) {
                    sig = fill_signal{true, false, s, px, ev.qty};
                }
                break;
            case event_type::delete_order:
                if (book_->peek_order(ev.order_id, s, px, q)) {
                    sig = fill_signal{true, false, s, px, q};  // full remaining qty
                }
                break;
            case event_type::replace:
                if (book_->peek_order(ev.order_id, s, px, q)) {
                    sig = fill_signal{true, false, s, px, q};  // cancel-leg of replace
                }
                break;
            case event_type::add:
                break;  // adds inject liquidity; no consumption
        }
        return sig;
    }

    [[nodiscard]] book_update snapshot(ts_t now) const noexcept {
        book_update u;
        u.timestamp   = now;
        u.two_sided   = true;  // only built when both sides present
        u.best_bid    = book_->best_bid();
        u.best_ask    = book_->best_ask();
        u.bid_qty     = book_->best_bid_qty();
        u.ask_qty     = book_->best_ask_qty();
        u.mid         = book_->mid();
        u.micro_price = book_->micro_price();
        u.imbalance   = book_->imbalance(1);
        return u;
    }

    hft_hot void drain_intents(ts_t now) noexcept {
        const std::size_t n = gw_.count();
        for (std::size_t i = 0; i < n; ++i) {
            const order_intent& it = gw_[i];
            switch (it.action) {
                case order_action::limit:
                    fm_->submit_limit(it.id, it.s, it.price, it.qty, now);
                    break;
                case order_action::market:
                    fm_->submit_market(it.id, it.s, it.qty, now);
                    break;
                case order_action::cancel:
                    (void)fm_->cancel(it.id);
                    break;
            }
        }
        gw_.clear();
    }

    Book*           book_;
    Strategy*       strat_;
    fill_model_t*   fm_;
    metrics_engine* metrics_;
    order_gateway   gw_;
    std::uint64_t   events_ = 0;
};

}  // namespace hft
