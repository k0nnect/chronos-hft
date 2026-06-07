// micro-price market maker.
//
// the maker posts passive quotes that *join the inside* (best bid / best ask) --
// that is where executable flow actually lands, so that is where a passive order
// earns fills and the maker rebate. the micro-price and top-level imbalance form
// a short-horizon "lean" signal: when it points strongly one way the maker pulls
// its quote on the side that would be run over (don't sell into a rising book,
// don't buy into a falling one). a hard inventory cap stops it from quoting a
// side that would breach the position limit, and quotes are only re-sent when
// something actually changes, so a quiet book produces no cancel/replace churn.
//
// derives from strategy<micro_price_mm>: every callback is resolved statically,
// with no virtual dispatch and no allocation on the path.
#pragma once

#include <cstdint>

#include "hft/core/compiler.hpp"
#include "hft/core/types.hpp"
#include "hft/engine/book_update.hpp"
#include "hft/engine/fill_model.hpp"
#include "hft/engine/strategy.hpp"

namespace hft {

class micro_price_mm : public strategy<micro_price_mm> {
public:
    struct config {
        qty_t        quote_size     = 100;   // shares per quote
        std::int64_t max_position   = 1000;  // hard inventory cap (shares)
        double       imb_lean       = 1.0;   // weight on imbalance in the lean signal
        double       lean_threshold = 0.6;   // |lean| past which we drop the weak side
    };

    explicit micro_price_mm(const config& cfg) noexcept : cfg_(cfg) {}

    // re-quote at the inside on every two-sided update.
    hft_hot void on_book_update(const book_update& u) noexcept {
        if (!u.two_sided) [[unlikely]] {
            return;
        }

        // lean > 0 => upward pressure (fair value above mid and/or bid-heavy book).
        const double mid  = 0.5 * static_cast<double>(u.best_bid + u.best_ask);
        const double lean = (u.micro_price - mid) + u.imbalance * cfg_.imb_lean;

        // inventory cap first: never quote a side that would breach the limit.
        bool quote_bid = net_position_ < cfg_.max_position;
        bool quote_ask = net_position_ > -cfg_.max_position;

        // signal skew: withdraw from the side the lean says is about to be run over.
        if (lean > cfg_.lean_threshold) {
            quote_ask = false;  // strong up pressure: stop offering
        } else if (lean < -cfg_.lean_threshold) {
            quote_bid = false;  // strong down pressure: stop bidding
        }

        requote(u.best_bid, u.best_ask, quote_bid, quote_ask);
    }

    // track our own inventory from fills and force a fresh quote on the filled side.
    hft_hot void on_order_fill(const fill_event& f) noexcept {
        if (f.s == side::bid) {
            net_position_ += static_cast<std::int64_t>(f.qty);
        } else {
            net_position_ -= static_cast<std::int64_t>(f.qty);
        }
        ++fills_;
        filled_qty_ += f.qty;
        if (f.order_id == bid_id_) bid_id_ = 0;
        if (f.order_id == ask_id_) ask_id_ = 0;
        quoted_ = false;
    }

    [[nodiscard]] std::int64_t  net_position() const noexcept { return net_position_; }
    [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }
    [[nodiscard]] qty_t         filled_qty() const noexcept { return filled_qty_; }
    [[nodiscard]] std::uint64_t requotes() const noexcept { return requotes_; }

private:
    hft_always_inline void requote(price_t bid_px, price_t ask_px, bool quote_bid,
                                   bool quote_ask) noexcept {
        if (quoted_ && bid_px == cur_bid_ && ask_px == cur_ask_ && quote_bid == q_bid_ &&
            quote_ask == q_ask_) [[likely]] {
            return;  // identical quotes already resting; leave them in place
        }

        if (bid_id_ != 0) this->cancel(bid_id_);
        if (ask_id_ != 0) this->cancel(ask_id_);
        bid_id_ = 0;
        ask_id_ = 0;

        if (quote_bid) bid_id_ = this->submit_limit(side::bid, bid_px, cfg_.quote_size);
        if (quote_ask) ask_id_ = this->submit_limit(side::ask, ask_px, cfg_.quote_size);

        cur_bid_ = bid_px;
        cur_ask_ = ask_px;
        q_bid_   = quote_bid;
        q_ask_   = quote_ask;
        quoted_  = true;
        ++requotes_;
    }

    config        cfg_;
    std::int64_t  net_position_ = 0;
    order_id_t    bid_id_       = 0;
    order_id_t    ask_id_       = 0;
    price_t       cur_bid_      = 0;
    price_t       cur_ask_      = 0;
    bool          q_bid_        = false;
    bool          q_ask_        = false;
    bool          quoted_       = false;
    std::uint64_t fills_        = 0;
    qty_t         filled_qty_   = 0;
    std::uint64_t requotes_     = 0;
};

}  // namespace hft
