// fixed-point market-impact model: temporary slippage + a relaxing permanent skew.
//
// motivation
// ----------
// the baseline fill model assumes infinite, static liquidity: a taker order pays
// exactly the displayed price at each level & our own flow never moves the
// market. that overstates passive edge & ignores the cost of size. this module
// supplies the two textbook components of execution cost, both in integer
// fixed-point so the hot path never touches the fpu:
//
//   * temporary impact -- an immediate, *linear* price concession charged while a
//     taker order depletes a single displayed level. taking the whole level costs
//     `temp_coeff` ticks; taking half of it costs half that. it models the
//     instantaneous book pressure of crossing & vanishes the moment our order
//     stops trading (hence "temporary": it is paid per child fill, not carried).
//
//   * permanent impact -- a *square-root law* displacement of the fair mid that
//     our trade leaves behind & that then relaxes over subsequent ticks. the
//     magnitude follows almgren's sqrt participation rule,
//         skew += gamma * sqrt(filled / rolling_volume),
//     signed by aggressor direction (a buy lifts the mid, a sell depresses it).
//     each event the accumulated skew is multiplied by a `decay` factor in [0,1),
//     so a single trade's footprint fades geometrically -- the propagator /
//     transient-impact picture, & the channel through which a large execution
//     produces measurable price decay on the marks that follow it.
//
// the rolling-volume denominator is an ewma of observed market trade size, floored
// by `default_volume` so the sqrt law is well-defined before any volume is seen
// & never divides by zero.
//
// fixed-point format (Q.16, matching the hardware feature datapath's FRAC=16):
//   a value v is stored as the integer round(v * 2^16). multiplies/divides go
//   through a 128-bit intermediate so intermediate products never overflow; the
//   square root is an exact integer (digit-by-digit) sqrt. no allocation, no
//   virtual calls, every operation branch-light & inlinable.
#pragma once

#include <cstdint>

#include "hft/core/compiler.hpp"
#include "hft/core/types.hpp"

namespace hft {

// low-level Q.16 fixed-point primitives. kept in their own namespace so callers
// (the fill model, the tests) can name the type & the round helper without
// pulling the whole model in.
namespace impact_detail {

inline constexpr unsigned kImpactFrac = 16;            // Q.16, as in feature_reference
using fp_t = std::int64_t;                             // signed Q47.16 fixed point
inline constexpr fp_t kImpactOne = fp_t{1} << kImpactFrac;

[[nodiscard]] hft_always_inline constexpr fp_t fp_from_int(std::int64_t v) noexcept {
    return v << kImpactFrac;
}

// configure-time only (cold): convert a human-supplied coefficient to fixed point
// with round-to-nearest. never called on the hot path.
[[nodiscard]] inline fp_t fp_from_double(double v) noexcept {
    const double scaled = v * static_cast<double>(kImpactOne);
    return static_cast<fp_t>(scaled + (v < 0.0 ? -0.5 : 0.5));
}

[[nodiscard]] hft_always_inline constexpr double fp_to_double(fp_t x) noexcept {
    return static_cast<double>(x) / static_cast<double>(kImpactOne);
}

// round a fixed-point value to the nearest whole integer (symmetric about zero).
[[nodiscard]] hft_always_inline constexpr std::int64_t fp_round(fp_t x) noexcept {
    const fp_t half = kImpactOne >> 1;
    return (x >= 0) ? ((x + half) >> kImpactFrac)
                    : -(((-x) + half) >> kImpactFrac);
}

[[nodiscard]] hft_always_inline constexpr fp_t fp_mul(fp_t a, fp_t b) noexcept {
    return static_cast<fp_t>((static_cast<__int128>(a) * static_cast<__int128>(b)) >> kImpactFrac);
}

[[nodiscard]] hft_always_inline constexpr fp_t fp_div(fp_t a, fp_t b) noexcept {
    if (b == 0) [[unlikely]] {
        return 0;
    }
    return static_cast<fp_t>((static_cast<__int128>(a) << kImpactFrac) / static_cast<__int128>(b));
}

// floor(sqrt(n)) for a 64-bit unsigned, computed digit-by-digit (no fpu, exact).
[[nodiscard]] hft_always_inline constexpr std::uint64_t isqrt_u64(std::uint64_t n) noexcept {
    std::uint64_t res = 0;
    std::uint64_t bit = std::uint64_t{1} << 62;  // highest even-power-of-two <= 2^63
    while (bit > n) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

// sqrt of a non-negative Q.16 value, result in Q.16. derivation:
//   want r with r/2^16 = sqrt(x/2^16)  =>  r = sqrt(x * 2^16) = isqrt(x << 16).
// the input is assumed bounded (participation ratios are small), so x << 16 fits
// in 64 bits with comfortable headroom.
[[nodiscard]] hft_always_inline constexpr fp_t fp_sqrt(fp_t x) noexcept {
    if (x <= 0) {
        return 0;
    }
    return static_cast<fp_t>(isqrt_u64(static_cast<std::uint64_t>(x) << kImpactFrac));
}

}  // namespace impact_detail

// human-facing knobs. coefficients are given as plain doubles (ticks / shares) &
// converted to fixed point once, at configure time; the hot path then runs purely
// on the integer `fp_t` representation. all-zero coefficients make the model inert,
// so a "baseline" run is just impact with these left at their defaults.
struct impact_config {
    double temp_coeff     = 0.0;  // ticks of temporary slippage at full-level depletion (kappa)
    double perm_coeff     = 0.0;  // ticks of permanent skew at unit sqrt-participation (gamma)
    double decay          = 1.0;  // per-event retention of the permanent skew in [0,1]; 1 = none
    double volume_alpha   = 0.05; // ewma weight for the rolling-volume metric in (0,1]
    double queue_coeff    = 0.0;  // shares of queue push-back at unit sqrt-participation
    qty_t  default_volume = 1;    // floor on the rolling-volume denominator (avoids /0, tiny denom)
    qty_t  initial_volume = 0;    // seed value for the rolling-volume ewma
};

// stateful impact accumulator. one lives inside each fill_model; it is a flat bag
// of scalars (no heap, no vtable) & every method is hot-path safe.
class market_impact {
public:
    using fp_t = impact_detail::fp_t;

    market_impact() = default;
    explicit market_impact(const impact_config& cfg) noexcept { configure(cfg); }

    // (cold) translate the double coefficients to fixed point & seed the rolling
    // volume. leaves the live permanent skew untouched so it can be reconfigured
    // mid-run; call reset() to clear accumulated state.
    void configure(const impact_config& cfg) noexcept {
        cfg_            = cfg;
        temp_coeff_     = impact_detail::fp_from_double(cfg.temp_coeff);
        perm_coeff_     = impact_detail::fp_from_double(cfg.perm_coeff);
        decay_factor_   = impact_detail::fp_from_double(cfg.decay);
        vol_alpha_      = impact_detail::fp_from_double(cfg.volume_alpha);
        queue_coeff_    = impact_detail::fp_from_double(cfg.queue_coeff);
        default_volume_ = cfg.default_volume != 0 ? cfg.default_volume : qty_t{1};
        vol_ewma_       = impact_detail::fp_from_int(static_cast<fp_t>(cfg.initial_volume));
    }

    [[nodiscard]] const impact_config& config() const noexcept { return cfg_; }

    // clear all accumulated state (permanent skew & rolling volume) back to the
    // configured seeds. coefficients are preserved.
    void reset() noexcept {
        perm_skew_ = 0;
        vol_ewma_  = impact_detail::fp_from_int(static_cast<fp_t>(cfg_.initial_volume));
    }

    // ---- temporary impact (charged per child fill during a sweep) ----------

    // adverse price concession, in fractional ticks, for taking `filled` shares out
    // of a level displaying `avail`. linear in the depletion fraction filled/avail,
    // so a full sweep of the level costs the whole `temp_coeff` & a partial take
    // costs proportionally less. the caller rounds this to whole ticks & pushes
    // the realized price the wrong way (a buyer pays more, a seller receives less).
    [[nodiscard]] hft_hot fp_t temporary_skew(qty_t filled, qty_t avail) const noexcept {
        if (avail == 0) [[unlikely]] {
            return 0;
        }
        const fp_t frac = impact_detail::fp_div(impact_detail::fp_from_int(static_cast<fp_t>(filled)),
                                                impact_detail::fp_from_int(static_cast<fp_t>(avail)));
        return impact_detail::fp_mul(temp_coeff_, frac);
    }

    // ---- permanent impact (accumulated, relaxes over subsequent ticks) -----

    // fold a completed taker execution of `filled` shares aggressing on side `s`
    // into the permanent mid skew via the square-root participation law. a buy
    // (aggressor on the bid) lifts the mid (+), a sell depresses it (-).
    hft_hot void apply_permanent(side s, qty_t filled) noexcept {
        if (filled == 0) [[unlikely]] {
            return;
        }
        const fp_t mag = impact_detail::fp_mul(perm_coeff_, participation_root(filled));
        perm_skew_ += (s == side::bid) ? mag : -mag;
    }

    // relax the accumulated permanent skew one event toward zero. with decay < 1
    // a single trade's displacement fades geometrically over the ticks that follow.
    hft_hot void decay() noexcept {
        perm_skew_ = impact_detail::fp_mul(perm_skew_, decay_factor_);
    }

    // update the rolling-volume ewma from an observed market trade of `traded`
    // shares. this is the denominator of the sqrt participation law.
    hft_hot void observe_volume(qty_t traded) noexcept {
        const fp_t t = impact_detail::fp_from_int(static_cast<fp_t>(traded));
        vol_ewma_ = impact_detail::fp_mul(vol_alpha_, t) +
                    impact_detail::fp_mul(impact_detail::kImpactOne - vol_alpha_, vol_ewma_);
    }

    // ---- queue degradation (adverse selection on resting orders) -----------

    // shares of queue push-back our resting passive orders suffer when our own
    // aggressive flow of `filled` shares prints. scaled by the same sqrt
    // participation term as the permanent skew: a larger footprint signals our
    // intent & draws faster liquidity ahead of us, so we wait longer for the
    // favourable fills & are left with the toxic ones.
    [[nodiscard]] hft_hot qty_t queue_degradation(qty_t filled) const noexcept {
        if (filled == 0) [[unlikely]] {
            return 0;
        }
        const fp_t shares = impact_detail::fp_mul(queue_coeff_, participation_root(filled));
        const std::int64_t r = impact_detail::fp_round(shares);
        return r > 0 ? static_cast<qty_t>(r) : qty_t{0};
    }

    // ---- read-only state ---------------------------------------------------

    // current permanent mid displacement: fixed-point ticks, & as a double.
    [[nodiscard]] fp_t   permanent_skew() const noexcept { return perm_skew_; }
    [[nodiscard]] double permanent_skew_ticks() const noexcept {
        return impact_detail::fp_to_double(perm_skew_);
    }

    // the live rolling-volume denominator (ewma, floored by default_volume).
    [[nodiscard]] qty_t rolling_volume() const noexcept {
        const std::int64_t v = impact_detail::fp_round(vol_ewma_);
        const qty_t        e = v > 0 ? static_cast<qty_t>(v) : qty_t{0};
        return e > default_volume_ ? e : default_volume_;
    }

private:
    // sqrt(filled / rolling_volume) in Q.16 -- the shared participation term of the
    // permanent skew & the queue degradation.
    [[nodiscard]] hft_always_inline fp_t participation_root(qty_t filled) const noexcept {
        const fp_t ratio =
            impact_detail::fp_div(impact_detail::fp_from_int(static_cast<fp_t>(filled)),
                                  impact_detail::fp_from_int(static_cast<fp_t>(rolling_volume())));
        return impact_detail::fp_sqrt(ratio);
    }

    impact_config cfg_{};

    // fixed-point coefficients (derived from cfg_ at configure time).
    fp_t temp_coeff_   = 0;
    fp_t perm_coeff_   = 0;
    fp_t decay_factor_ = impact_detail::kImpactOne;  // 1.0 == no relaxation
    fp_t vol_alpha_    = 0;
    fp_t queue_coeff_  = 0;
    qty_t default_volume_ = 1;

    // accumulated live state.
    fp_t perm_skew_ = 0;  // signed permanent mid displacement, Q.16 ticks
    fp_t vol_ewma_  = 0;  // rolling trade-volume ewma, Q.16 shares
};

}  // namespace hft
