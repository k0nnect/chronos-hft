// deterministic, queue-position-aware fill simulator.
//
// our simulated orders are *not* inserted into the (feed-driven) order book --
// that would corrupt the replay, since later feed messages reference real ids.
// instead each working order tracks how much real volume sits ahead of it in the
// queue (`queue_ahead`) and only fills once that volume has traded through. this
// is the single most important detail for a realistic passive backtest: without
// it a maker strategy "fills" on every print and shows fantasy p&l.
//
// model summary
// -------------
// * execution latency: a submitted order does not become active until
//   submit_time + latency_ns (tick-to-trade). it cannot fill before then.
// * passive limit: on activation its queue_ahead is the resting volume already
//   at its price/side. a *trade* at that price/side first consumes queue_ahead,
//   then fills us (partial fills supported). a *cancel/delete* at that price/side
//   only shrinks queue_ahead (it improves our position but is not a trade).
// * marketable limit / market order: on activation it sweeps displayed liquidity
//   on the opposite side up to its limit (market = no limit), paying the taker
//   fee. a market remainder is dropped (ioc); a marketable-limit remainder rests
//   passively at its limit.
// * fees: maker/taker basis points on notional (price*qty*tick_value); a negative
//   maker bp is a rebate.
//
// fixed-capacity, allocation-free, no virtual calls. the fill sink is a template
// parameter so emitting a fill is a direct inlined call.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "hft/core/compiler.hpp"
#include "hft/core/types.hpp"

namespace hft {

// a single simulated execution delivered to the strategy and the metrics engine.
struct fill_event {
    ts_t       timestamp = 0;
    order_id_t order_id  = 0;      // our client order id (from the gateway)
    price_t    price     = 0;      // execution price in ticks
    qty_t      qty       = 0;      // filled quantity
    side       s         = side::bid;
    bool       maker     = false;  // true = passive/maker, false = aggressor/taker
    double     fee       = 0.0;    // signed currency: + cost, - rebate
};

// per-event consumption signal resolved by the engine from a market_event:
// `is_trade` distinguishes real volume (can fill us) from a pure cancel.
struct fill_signal {
    bool    valid    = false;
    bool    is_trade = false;
    side    s        = side::bid;
    price_t price    = 0;
    qty_t   qty      = 0;
};

struct fill_config {
    ts_t   latency_ns    = 0;     // tick-to-trade delay before an order is live
    double maker_fee_bps = 0.0;   // basis points on notional; negative = rebate
    double taker_fee_bps = 0.0;   // basis points on notional
    double tick_value    = 1.0;   // currency value of one price tick per share
};

template <std::size_t MaxWorking = 256>
class fill_model {
    static_assert(MaxWorking > 0, "need room for at least one working order");
    static constexpr std::size_t kMaxSweepLevels = 64;

    struct working_order {
        order_id_t id;
        ts_t       activation_time;
        price_t    price;        // no_price for a market order
        qty_t      remaining;
        qty_t      queue_ahead;
        side       s;
        bool       is_market;
        bool       active;
    };

public:
    fill_model() = default;
    explicit fill_model(const fill_config& cfg) noexcept : cfg_(cfg) {}

    void configure(const fill_config& cfg) noexcept { cfg_ = cfg; }
    [[nodiscard]] const fill_config& config() const noexcept { return cfg_; }

    void reset() noexcept { n_ = 0; }
    [[nodiscard]] std::size_t working_count() const noexcept { return n_; }

    // register a passive/limit order. queue position is finalised at activation
    // (using the book at that later moment), which is more faithful than fixing it
    // at submit time across the latency gap.
    hft_hot void submit_limit(order_id_t id, side s, price_t price, qty_t qty,
                              ts_t now) noexcept {
        push(working_order{id, now + cfg_.latency_ns, price, qty, 0, s, false, false});
    }

    hft_hot void submit_market(order_id_t id, side s, qty_t qty, ts_t now) noexcept {
        push(working_order{id, now + cfg_.latency_ns, no_price, qty, 0, s, true, false});
    }

    // remove a working order by id (whether or not it has activated). returns
    // true if it was found.
    hft_hot bool cancel(order_id_t id) noexcept {
        for (std::size_t i = 0; i < n_; ++i) {
            if (orders_[i].id == id) {
                orders_[i] = orders_[--n_];  // swap-remove keeps the array dense
                return true;
            }
        }
        return false;
    }

    // advance the simulator one event: activate any orders whose latency has
    // elapsed (sweeping marketable ones immediately), then apply this event's
    // consumption signal to resting passive orders. fills are pushed to `sink`.
    template <typename Book, typename Sink>
    hft_hot void on_event(ts_t now, const fill_signal& sig, const Book& book,
                          Sink&& sink) noexcept {
        const double mk_bps = cfg_.maker_fee_bps;
        const double tk_bps = cfg_.taker_fee_bps;
        const double tv     = cfg_.tick_value;

        auto emit = [&](const working_order& wo, price_t px, qty_t q, bool maker) {
            const double notional = static_cast<double>(px) * static_cast<double>(q) * tv;
            const double fee      = notional * (maker ? mk_bps : tk_bps) / 10000.0;
            sink(fill_event{now, wo.id, px, q, wo.s, maker, fee});
        };

        // sweep displayed liquidity on the opposite side for a (marketable) order.
        auto taker_sweep = [&](working_order& wo) {
            const side    opp   = opposite(wo.s);
            const price_t limit = wo.is_market
                                      ? (wo.s == side::bid ? std::numeric_limits<price_t>::max()
                                                           : std::numeric_limits<price_t>::min())
                                      : wo.price;
            qty_t remaining = wo.remaining;
            book.walk(opp, kMaxSweepLevels, [&](price_t px, qty_t avail) -> bool {
                if (wo.s == side::bid && px > limit) return false;   // ask too dear
                if (wo.s == side::ask && px < limit) return false;   // bid too low
                const qty_t f = std::min(remaining, avail);
                if (f != 0) {
                    emit(wo, px, f, /*maker=*/false);
                    remaining -= f;
                }
                return remaining != 0;
            });
            wo.remaining = remaining;
        };

        std::size_t w = 0;  // write cursor for surviving orders (compaction)
        for (std::size_t r = 0; r < n_; ++r) {
            working_order wo = orders_[r];

            // --- activation (after the tick-to-trade latency) ---
            if (!wo.active && wo.activation_time <= now) [[unlikely]] {
                wo.active = true;
                if (wo.is_market || is_marketable(wo, book)) {
                    taker_sweep(wo);
                    if (wo.is_market) {
                        continue;  // ioc: drop any unfilled market remainder
                    }
                    if (wo.remaining == 0) {
                        continue;  // marketable limit fully filled
                    }
                    // marketable-limit remainder now rests passively at its price.
                    wo.queue_ahead = book.level_qty(wo.s, wo.price);
                } else {
                    wo.queue_ahead = book.level_qty(wo.s, wo.price);
                }
            }

            // --- passive fill / queue update from this event's signal ---
            if (wo.active && !wo.is_market && sig.valid && wo.s == sig.s &&
                wo.price == sig.price) [[unlikely]] {
                if (sig.is_trade) {
                    if (wo.queue_ahead >= sig.qty) {
                        wo.queue_ahead -= sig.qty;  // all volume hit the queue ahead
                    } else {
                        const qty_t reaching = sig.qty - wo.queue_ahead;
                        wo.queue_ahead       = 0;
                        const qty_t f        = std::min(reaching, wo.remaining);
                        wo.remaining -= f;
                        emit(wo, wo.price, f, /*maker=*/true);
                    }
                } else {
                    // a cancel/delete ahead of us only improves our position.
                    wo.queue_ahead = (wo.queue_ahead >= sig.qty) ? wo.queue_ahead - sig.qty : 0;
                }
            }

            if (wo.remaining != 0) {
                orders_[w++] = wo;  // keep survivors
            }
        }
        n_ = w;
    }

private:
    hft_always_inline void push(const working_order& wo) noexcept {
        if (n_ >= MaxWorking) [[unlikely]] {
            return;  // capacity guard: silently refuse rather than overrun
        }
        orders_[n_++] = wo;
    }

    template <typename Book>
    [[nodiscard]] hft_always_inline static bool is_marketable(const working_order& wo,
                                                              const Book& book) noexcept {
        if (wo.s == side::bid) {
            return book.has_ask() && wo.price >= book.best_ask();
        }
        return book.has_bid() && wo.price <= book.best_bid();
    }

    fill_config                            cfg_{};
    std::size_t                            n_ = 0;
    std::array<working_order, MaxWorking>  orders_{};
};

}  // namespace hft
