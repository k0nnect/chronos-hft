// statistical market maker: a depth-aware, inventory-managed quoting strategy.
//
// it reads a multi-level order-book imbalance through the alpha_engine (a
// volume-weighted obi smoothed by a constant-gain kalman filter) & turns that,
// together with its own inventory, into two integer Q.16 controls:
//
//   * skew  -- shift the quote center. lean *with* the predicted move (alpha) to
//              earn the drift, & lean *against* inventory to mean-revert the book
//              risk back toward flat.
//   * width -- widen the half-spread when the alpha signal is strong or inventory
//              is large. strong alpha means the resting side is about to be
//              adversely selected (the very toxicity the market-impact module
//              models as permanent skew + queue degradation), so we demand more
//              edge before resting there; heavy inventory likewise wants a wider,
//              safer quote.
//
// a hard position cap stops it quoting a side that would breach the limit, &
// quotes are only re-sent when they actually change, so a quiet book is silent.
//
// derives from strategy<statistical_mm>: callbacks are resolved statically (no
// vtable). all hot-path math is integer Q.16 with zero heap allocation; the cold
// double->fixed-point configuration lives out of line in statistical_mm.cpp.
#pragma once

#include <cstdint>

#include "hft/core/compiler.hpp"
#include "hft/core/types.hpp"
#include "hft/engine/book_update.hpp"
#include "hft/engine/fill_model.hpp"
#include "hft/engine/strategy.hpp"
#include "hft/metrics/trace_extras.hpp"
#include "hft/signals/alpha_engine.hpp"

namespace hft {

// bring the fixed-point helpers into this namespace under a short alias.
namespace q16 = signals::q16;

class statistical_mm : public strategy<statistical_mm> {
public:
    struct config {
        qty_t        quote_size       = 100;   // shares per quote
        std::int64_t max_position     = 2000;  // hard inventory cap (shares)
        double       base_half_spread = 2.0;   // ticks at flat inventory & flat alpha
        double       inv_skew         = 3.0;   // ticks of center skew at full inventory
        double       inv_widen        = 2.0;   // extra half-spread ticks at full inventory
        double       alpha_skew       = 4.0;   // ticks of center skew at |alpha| = 1
        double       alpha_widen      = 3.0;   // extra half-spread ticks at |alpha| = 1
        double       gain_alpha       = 0.30;  // alpha-beta level gain
        double       gain_beta        = 0.05;  // alpha-beta velocity gain
    };

    // defined out of line (cold double -> Q.16 conversion) in statistical_mm.cpp.
    explicit statistical_mm(const config& cfg) noexcept;

    // the default parameter set, also defined in statistical_mm.cpp.
    [[nodiscard]] static config default_config() noexcept;

    // re-quote on every two-sided update, skewed & widened by alpha + inventory.
    hft_hot void on_book_update(const book_update& u) noexcept {
        if (!u.two_sided) [[unlikely]] {
            return;
        }

        const q16::fp_t alpha  = alpha_.on_book(u);     // filtered alpha in [-1,1]
        const q16::fp_t fair   = micro_q16(u);          // integer micro-price (Q.16 ticks)
        const q16::fp_t inv    = inventory_ratio();     // position / cap in [-1,1]

        // half-width: base + adverse-selection (|alpha|) + inventory cushion.
        const q16::fp_t half_spread = base_hs_ +
                                      q16::mul(alpha_widen_, q16::fabs(alpha)) +
                                      q16::mul(inv_widen_, q16::fabs(inv));

        // center skew: lean with alpha, lean against inventory.
        const q16::fp_t skew   = q16::mul(alpha_skew_, alpha) - q16::mul(inv_skew_, inv);
        const q16::fp_t center = fair + skew;

        price_t bid_px = q16::round(center - half_spread);
        price_t ask_px = q16::round(center + half_spread);

        // a maker never quotes *tighter* than the touch: a tighter computed price
        // simply joins the inside, & widening / skew only ever step the quote
        // further out (resting it behind the touch) -- the lever against adverse
        // selection & queue degradation. so clamp each side to the touch or worse.
        if (bid_px > u.best_bid) bid_px = u.best_bid;
        if (ask_px < u.best_ask) ask_px = u.best_ask;
        if (ask_px <= bid_px) [[unlikely]] ask_px = bid_px + 1;  // book is uncrossed: paranoia

        // inventory cap: never offer a side that would breach the position limit.
        const bool quote_bid = pos_ < cfg_.max_position;
        const bool quote_ask = pos_ > -cfg_.max_position;

        requote(bid_px, ask_px, quote_bid, quote_ask);
        publish_trace(alpha, inv, half_spread, skew);
    }

    // track our own inventory from fills & force a fresh quote on the filled side.
    hft_hot void on_order_fill(const fill_event& f) noexcept {
        if (f.s == side::bid) {
            pos_ += static_cast<std::int64_t>(f.qty);
        } else {
            pos_ -= static_cast<std::int64_t>(f.qty);
        }
        ++fills_;
        filled_qty_ += f.qty;
        if (f.order_id == bid_id_) bid_id_ = 0;
        if (f.order_id == ask_id_) ask_id_ = 0;
        quoted_ = false;
    }

    // ---- introspection (stats & trace) ------------------------------------
    [[nodiscard]] std::int64_t          net_position() const noexcept { return pos_; }
    [[nodiscard]] std::uint64_t         fills() const noexcept { return fills_; }
    [[nodiscard]] qty_t                 filled_qty() const noexcept { return filled_qty_; }
    [[nodiscard]] std::uint64_t         requotes() const noexcept { return requotes_; }
    [[nodiscard]] const trace_extras&   trace_state() const noexcept { return tr_; }
    [[nodiscard]] const signals::alpha_engine<>& alpha_engine() const noexcept { return alpha_; }

private:
    // cold setup: translate the double config into the Q.16 coefficients the hot
    // path uses. defined in statistical_mm.cpp.
    void configure(const config& cfg) noexcept;

    // size-weighted micro-price in Q.16 ticks: bid weighted by ask size & vice
    // versa. falls back to the arithmetic mid when a side shows no size.
    [[nodiscard]] hft_always_inline q16::fp_t micro_q16(const book_update& u) const noexcept {
        const std::int64_t bb = u.best_bid;
        const std::int64_t ba = u.best_ask;
        const std::int64_t bq = static_cast<std::int64_t>(u.bid_qty);
        const std::int64_t aq = static_cast<std::int64_t>(u.ask_qty);
        const std::int64_t denom = bq + aq;
        if (denom <= 0) [[unlikely]] {
            return static_cast<q16::fp_t>((bb + ba) << (q16::frac - 1));  // mid in Q.16
        }
        const __int128 numer = static_cast<__int128>(bb) * aq + static_cast<__int128>(ba) * bq;
        return static_cast<q16::fp_t>((numer << q16::frac) / denom);
    }

    // signed inventory as a fraction of the cap, in Q.16, clamped to [-1, 1].
    [[nodiscard]] hft_always_inline q16::fp_t inventory_ratio() const noexcept {
        if (cfg_.max_position <= 0) [[unlikely]] {
            return 0;
        }
        const q16::fp_t r = static_cast<q16::fp_t>(
            (static_cast<__int128>(pos_) << q16::frac) / cfg_.max_position);
        return q16::clamp(r, -q16::one, q16::one);
    }

    hft_always_inline void publish_trace(q16::fp_t alpha, q16::fp_t inv, q16::fp_t half_spread,
                                         q16::fp_t skew) noexcept {
        tr_.obi         = alpha_.obi();
        tr_.alpha       = q16::to_double(alpha);
        tr_.velocity    = alpha_.velocity();
        tr_.half_spread = q16::to_double(half_spread);
        tr_.skew        = q16::to_double(skew);
        tr_.inventory   = q16::to_double(inv);
        tr_.present     = true;
    }

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

    config                  cfg_;
    signals::alpha_engine<> alpha_;

    // Q.16 coefficients derived from cfg_ at configure time.
    q16::fp_t base_hs_     = 0;
    q16::fp_t inv_skew_    = 0;
    q16::fp_t inv_widen_   = 0;
    q16::fp_t alpha_skew_  = 0;
    q16::fp_t alpha_widen_ = 0;

    std::int64_t  pos_         = 0;
    order_id_t    bid_id_      = 0;
    order_id_t    ask_id_      = 0;
    price_t       cur_bid_     = 0;
    price_t       cur_ask_     = 0;
    bool          q_bid_       = false;
    bool          q_ask_       = false;
    bool          quoted_      = false;
    std::uint64_t fills_       = 0;
    std::uint64_t requotes_    = 0;
    qty_t         filled_qty_  = 0;
    trace_extras  tr_;
};

}  // namespace hft
