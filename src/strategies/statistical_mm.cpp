// out-of-line (cold) parts of the statistical market maker: translating the
// human-facing double config into the Q.16 coefficients the hot path runs on, &
// the default parameter set. kept here so the per-event path in the header carries
// no floating-point setup work.
#include "hft/strategies/statistical_mm.hpp"

namespace hft {

void statistical_mm::configure(const config& cfg) noexcept {
    cfg_         = cfg;
    base_hs_     = q16::from_double(cfg.base_half_spread);
    inv_skew_    = q16::from_double(cfg.inv_skew);
    inv_widen_   = q16::from_double(cfg.inv_widen);
    alpha_skew_  = q16::from_double(cfg.alpha_skew);
    alpha_widen_ = q16::from_double(cfg.alpha_widen);
    alpha_.configure(signals::alpha_engine<>::config{cfg.gain_alpha, cfg.gain_beta});
}

statistical_mm::statistical_mm(const config& cfg) noexcept { configure(cfg); }

statistical_mm::config statistical_mm::default_config() noexcept { return config{}; }

}  // namespace hft
