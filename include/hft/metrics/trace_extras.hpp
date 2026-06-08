// a small, fixed bundle of strategy-internal signals the trace logger appends to
// each snapshot, so the replay dashboard can show alpha & risk state alongside the
// book. plain doubles, trivially copyable, no allocation -- a strategy fills one
// of these each tick & the logger reads it through a borrowed pointer.
#pragma once

namespace hft {

struct trace_extras {
    double obi         = 0.0;  // raw volume-weighted order-book imbalance [-1,1]
    double alpha       = 0.0;  // filtered alpha (constant-gain kalman level)
    double velocity    = 0.0;  // alpha tracking velocity
    double half_spread = 0.0;  // current quoting half-width (ticks)
    double skew        = 0.0;  // current center skew (ticks)
    double inventory   = 0.0;  // inventory ratio in [-1,1]
    bool   present     = false;  // false => the logger omits the "a" object
};

}  // namespace hft
