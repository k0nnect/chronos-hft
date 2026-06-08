// allocation-free, O(1)-per-update trading metrics.
//
// everything is a running scalar -- no per-trade history vector, nothing to grow
// on the hot path. position uses signed average-cost accounting (handles adds,
// reductions, closes & flips); equity is marked each tick; drawdown tracks the
// running peak; & a welford accumulator gives mean/variance of per-tick equity
// changes for a sharpe estimate without storing the series.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "hft/core/compiler.hpp"
#include "hft/engine/fill_model.hpp"

namespace hft {

struct metrics_config {
    double tick_value = 1.0;  // currency value of one price tick per share
};

class metrics_engine {
public:
    metrics_engine() = default;
    explicit metrics_engine(const metrics_config& cfg) noexcept : cfg_(cfg) {}

    void configure(const metrics_config& cfg) noexcept { cfg_ = cfg; }

    // fold one execution into position, realized p&l & fees. uses signed
    // average-cost: building a position updates the average price; reducing or
    // crossing through zero realizes p&l on the closed quantity.
    hft_hot void on_fill(const fill_event& f) noexcept {
        const std::int64_t signed_qty =
            (f.s == side::bid) ? static_cast<std::int64_t>(f.qty)
                               : -static_cast<std::int64_t>(f.qty);
        const double px = static_cast<double>(f.price);

        if (position_ == 0 || (position_ > 0) == (signed_qty > 0)) {
            // opening or adding in the same direction: roll the average price.
            const std::int64_t new_pos = position_ + signed_qty;
            const double prev_abs = static_cast<double>(std::llabs(position_));
            const double add_abs  = static_cast<double>(std::llabs(signed_qty));
            avg_price_ = (avg_price_ * prev_abs + px * add_abs) /
                         static_cast<double>(std::llabs(new_pos));
            position_ = new_pos;
        } else {
            // reducing, closing, or flipping: realize on the closed quantity.
            const std::int64_t old_pos = position_;
            const std::int64_t closing =
                std::min(std::llabs(signed_qty), std::llabs(position_));
            const double pnl_per = (position_ > 0) ? (px - avg_price_) : (avg_price_ - px);
            realized_ += pnl_per * static_cast<double>(closing) * cfg_.tick_value;
            position_ += signed_qty;
            if (position_ == 0) {
                avg_price_ = 0.0;
            } else if ((position_ > 0) != (old_pos > 0)) {
                avg_price_ = px;  // flipped through zero: new leg opens at fill px
            }
        }

        realized_ -= f.fee;  // fees reduce p&l (a negative fee is a rebate credit)
        fees_ += f.fee;
        ++fills_;
        volume_ += f.qty;
        const std::uint64_t inv = static_cast<std::uint64_t>(std::llabs(position_));
        if (inv > peak_inventory_) peak_inventory_ = inv;
    }

    // mark the book to a reference price (ticks) & roll equity-derived stats.
    hft_hot void on_mark(double mark_ticks) noexcept {
        unrealized_ = static_cast<double>(position_) * (mark_ticks - avg_price_) * cfg_.tick_value;
        equity_     = realized_ + unrealized_;

        if (equity_ > peak_equity_) peak_equity_ = equity_;
        const double dd = peak_equity_ - equity_;
        if (dd > max_drawdown_) max_drawdown_ = dd;

        // welford update on the per-tick equity change.
        const double delta = equity_ - last_equity_;
        last_equity_       = equity_;
        ++samples_;
        const double d = delta - ret_mean_;
        ret_mean_ += d / static_cast<double>(samples_);
        ret_m2_ += d * (delta - ret_mean_);
    }

    // ---- accessors --------------------------------------------------------
    [[nodiscard]] std::int64_t  position() const noexcept { return position_; }
    [[nodiscard]] double        avg_price() const noexcept { return avg_price_; }
    [[nodiscard]] double        realized_pnl() const noexcept { return realized_; }
    [[nodiscard]] double        unrealized_pnl() const noexcept { return unrealized_; }
    [[nodiscard]] double        equity() const noexcept { return equity_; }
    [[nodiscard]] double        fees() const noexcept { return fees_; }
    [[nodiscard]] double        max_drawdown() const noexcept { return max_drawdown_; }
    [[nodiscard]] std::uint64_t peak_inventory() const noexcept { return peak_inventory_; }
    [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }
    [[nodiscard]] qty_t         volume() const noexcept { return volume_; }

    // per-tick sharpe estimate (mean / stdev of equity increments). multiply by
    // sqrt(ticks per period) externally to annualise.
    [[nodiscard]] double sharpe() const noexcept {
        if (samples_ < 2) return 0.0;
        const double var = ret_m2_ / static_cast<double>(samples_ - 1);
        const double sd  = std::sqrt(var);
        return sd > 0.0 ? ret_mean_ / sd : 0.0;
    }

private:
    metrics_config cfg_{};

    std::int64_t  position_       = 0;
    double        avg_price_      = 0.0;
    double        realized_       = 0.0;
    double        unrealized_     = 0.0;
    double        equity_         = 0.0;
    double        fees_           = 0.0;
    double        peak_equity_    = 0.0;
    double        max_drawdown_   = 0.0;
    std::uint64_t peak_inventory_ = 0;
    std::uint64_t fills_          = 0;
    qty_t         volume_         = 0;

    // welford accumulator for equity-increment mean/variance.
    std::uint64_t samples_     = 0;
    double        ret_mean_    = 0.0;
    double        ret_m2_      = 0.0;
    double        last_equity_ = 0.0;
};

}  // namespace hft
