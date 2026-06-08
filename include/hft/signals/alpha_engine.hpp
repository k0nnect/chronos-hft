// statistical alpha engine: a multi-level order-book imbalance signal smoothed by
// a constant-gain tracking filter, all in integer Q.16 fixed point.
//
// two pieces:
//   1. order-book imbalance (obi) -- a volume-weighted imbalance over the top
//      `Depth` levels of each side. nearer levels carry more weight (a linear
//      taper {Depth, Depth-1, ..., 1}), since flow near the touch is the most
//      predictive. the result is signed & bounded to [-1, 1] in Q.16: +1 is an
//      entirely bid-heavy book, -1 entirely ask-heavy.
//   2. a constant-gain alpha-beta filter -- the steady-state form of a kalman
//      filter that tracks a level & a velocity with fixed gains. it denoises the
//      raw obi (which jumps tick to tick) into a stable alpha estimate without
//      storing any history or allocating: two state words, two multiplies & two
//      adds per update.
//
// every operation is integer Q.16 (the same FRAC=16 format the rest of the system
// uses); doubles appear only at configure time to translate human-set gains. no
// heap, no virtual calls, branch-light.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "hft/core/compiler.hpp"
#include "hft/core/types.hpp"
#include "hft/engine/book_update.hpp"

namespace hft::signals {

// low-level Q.16 fixed-point primitives, mirroring the impact module's format so
// the whole system speaks one fixed-point dialect.
namespace q16 {

using fp_t = std::int64_t;                       // signed Q47.16 fixed point
inline constexpr unsigned frac = 16;
inline constexpr fp_t     one  = fp_t{1} << frac;

[[nodiscard]] hft_always_inline constexpr fp_t from_int(std::int64_t v) noexcept {
    return v << frac;
}

// configure-time only (cold): round-to-nearest a human-supplied coefficient.
[[nodiscard]] inline fp_t from_double(double v) noexcept {
    const double scaled = v * static_cast<double>(one);
    return static_cast<fp_t>(scaled + (v < 0.0 ? -0.5 : 0.5));
}

[[nodiscard]] hft_always_inline constexpr double to_double(fp_t x) noexcept {
    return static_cast<double>(x) / static_cast<double>(one);
}

[[nodiscard]] hft_always_inline constexpr fp_t mul(fp_t a, fp_t b) noexcept {
    return static_cast<fp_t>((static_cast<__int128>(a) * static_cast<__int128>(b)) >> frac);
}

[[nodiscard]] hft_always_inline constexpr fp_t div(fp_t a, fp_t b) noexcept {
    if (b == 0) [[unlikely]] {
        return 0;
    }
    return static_cast<fp_t>((static_cast<__int128>(a) << frac) / static_cast<__int128>(b));
}

// round a Q.16 value to the nearest whole integer (symmetric about zero).
[[nodiscard]] hft_always_inline constexpr std::int64_t round(fp_t x) noexcept {
    const fp_t half = one >> 1;
    return (x >= 0) ? ((x + half) >> frac) : -(((-x) + half) >> frac);
}

[[nodiscard]] hft_always_inline constexpr fp_t fabs(fp_t x) noexcept { return x < 0 ? -x : x; }

[[nodiscard]] hft_always_inline constexpr fp_t clamp(fp_t x, fp_t lo, fp_t hi) noexcept {
    return x < lo ? lo : (x > hi ? hi : x);
}

}  // namespace q16

template <std::size_t Depth = book_update_depth>
class alpha_engine {
    static_assert(Depth > 0, "need at least one level");
    static_assert(Depth <= book_update_depth, "Depth exceeds the snapshot's level count");

public:
    struct config {
        double gain_alpha = 0.30;  // level gain of the constant-gain (kalman) filter
        double gain_beta  = 0.05;  // velocity gain
    };

    alpha_engine() noexcept { configure(config{}); }
    explicit alpha_engine(const config& cfg) noexcept { configure(cfg); }

    // (cold) translate gains to Q.16 & seed the per-level weight taper.
    void configure(const config& cfg) noexcept {
        ga_ = q16::from_double(cfg.gain_alpha);
        gb_ = q16::from_double(cfg.gain_beta);
        for (std::size_t k = 0; k < Depth; ++k) {
            w_[k] = static_cast<std::int64_t>(Depth - k);  // nearer levels weigh most
        }
    }

    void reset() noexcept {
        level_    = 0;
        vel_      = 0;
        last_obi_ = 0;
        primed_   = false;
    }

    // volume-weighted order-book imbalance in Q.16, bounded to [-1, 1]. `nb`/`na`
    // are how many of the level arrays are populated; missing levels count as zero.
    [[nodiscard]] hft_hot q16::fp_t compute_obi(const qty_t* bid_sz, std::size_t nb,
                                                const qty_t* ask_sz, std::size_t na) const noexcept {
        std::int64_t num = 0;
        std::int64_t den = 0;
        for (std::size_t k = 0; k < Depth; ++k) {
            const std::int64_t w = w_[k];
            const std::int64_t b = (k < nb) ? static_cast<std::int64_t>(bid_sz[k]) : 0;
            const std::int64_t a = (k < na) ? static_cast<std::int64_t>(ask_sz[k]) : 0;
            num += w * (b - a);
            den += w * (b + a);
        }
        if (den <= 0) [[unlikely]] {
            return 0;
        }
        const q16::fp_t obi =
            static_cast<q16::fp_t>((static_cast<__int128>(num) << q16::frac) / den);
        return q16::clamp(obi, -q16::one, q16::one);
    }

    // one constant-gain alpha-beta update against a fresh obi sample; returns the
    // filtered level. the first sample primes the filter (no transient ramp-in).
    hft_hot q16::fp_t step(q16::fp_t obi) noexcept {
        last_obi_ = obi;
        if (!primed_) [[unlikely]] {
            level_  = obi;
            vel_    = 0;
            primed_ = true;
            return level_;
        }
        const q16::fp_t pred  = level_ + vel_;        // predict one step ahead
        const q16::fp_t resid = obi - pred;           // innovation
        level_ = q16::clamp(pred + q16::mul(ga_, resid), -q16::one, q16::one);
        vel_   = q16::clamp(vel_ + q16::mul(gb_, resid), -q16::one, q16::one);
        return level_;
    }

    // convenience: compute the obi from a snapshot's depth arrays & filter it.
    hft_hot q16::fp_t on_book(const book_update& u) noexcept {
        return step(compute_obi(u.bid_sz, u.bid_levels, u.ask_sz, u.ask_levels));
    }

    // ---- state accessors (Q.16 & double views) ----------------------------
    [[nodiscard]] q16::fp_t obi_q16() const noexcept { return last_obi_; }
    [[nodiscard]] q16::fp_t alpha_q16() const noexcept { return level_; }
    [[nodiscard]] q16::fp_t velocity_q16() const noexcept { return vel_; }
    [[nodiscard]] double    obi() const noexcept { return q16::to_double(last_obi_); }
    [[nodiscard]] double    alpha() const noexcept { return q16::to_double(level_); }
    [[nodiscard]] double    velocity() const noexcept { return q16::to_double(vel_); }
    [[nodiscard]] bool      primed() const noexcept { return primed_; }

private:
    q16::fp_t                       ga_ = 0;  // level gain (Q.16)
    q16::fp_t                       gb_ = 0;  // velocity gain (Q.16)
    std::array<std::int64_t, Depth> w_{};     // per-level integer weights

    q16::fp_t level_    = 0;  // filtered alpha (Q.16)
    q16::fp_t vel_      = 0;  // tracking velocity (Q.16)
    q16::fp_t last_obi_ = 0;  // most recent raw obi (Q.16)
    bool      primed_   = false;
};

}  // namespace hft::signals
