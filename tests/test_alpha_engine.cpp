// tests for the statistical alpha engine: the volume-weighted, depth-weighted
// order-book imbalance (sign, bounds & near-level dominance) & the constant-gain
// alpha-beta tracking filter (priming, steady state, monotone convergence).
#include <cstddef>

#include "check.hpp"
#include "hft/engine/book_update.hpp"
#include "hft/signals/alpha_engine.hpp"

using namespace hft;
namespace q16 = hft::signals::q16;

namespace {

bool approx(double a, double b, double eps = 1e-4) {
    const double d = a - b;
    return (d < 0 ? -d : d) < eps;
}

// a fully bid-heavy book reads +1, fully ask-heavy reads -1, symmetric reads 0.
void obi_sign_and_bounds() {
    signals::alpha_engine<5> eng;

    const qty_t all_bid_b[5] = {100, 50, 25, 10, 5};
    const qty_t all_bid_a[5] = {0, 0, 0, 0, 0};
    check(approx(q16::to_double(eng.compute_obi(all_bid_b, 5, all_bid_a, 5)), 1.0));

    const qty_t all_ask_b[5] = {0, 0, 0, 0, 0};
    const qty_t all_ask_a[5] = {100, 50, 25, 10, 5};
    check(approx(q16::to_double(eng.compute_obi(all_ask_b, 5, all_ask_a, 5)), -1.0));

    const qty_t sym[5] = {100, 80, 60, 40, 20};
    check(approx(q16::to_double(eng.compute_obi(sym, 5, sym, 5)), 0.0));

    // an empty book is neutral, not a divide-by-zero.
    const qty_t zero[5] = {0, 0, 0, 0, 0};
    check_eq(eng.compute_obi(zero, 5, zero, 5), 0);
}

// the taper weights near levels more heavily than deep ones.
void obi_near_levels_dominate() {
    signals::alpha_engine<5> eng;

    // bid sits at the touch, the matching ask sits five levels deep.
    const qty_t near_bid_b[5] = {100, 0, 0, 0, 0};
    const qty_t near_bid_a[5] = {0, 0, 0, 0, 100};
    const double near_bid = q16::to_double(eng.compute_obi(near_bid_b, 5, near_bid_a, 5));

    // the mirror image: ask at the touch, bid five levels deep.
    const qty_t near_ask_b[5] = {0, 0, 0, 0, 100};
    const qty_t near_ask_a[5] = {100, 0, 0, 0, 0};
    const double near_ask = q16::to_double(eng.compute_obi(near_ask_b, 5, near_ask_a, 5));

    check(near_bid > 0.0);          // touch-heavy bid => positive
    check(near_ask < 0.0);          // touch-heavy ask => negative
    check(approx(near_bid, -near_ask));  // symmetric in magnitude
    // weights {5,4,3,2,1}: (5*100 - 1*100)/(5*100 + 1*100) = 400/600.
    check(approx(near_bid, 400.0 / 600.0));
}

// the first sample primes the filter; a constant input is a fixed point.
void filter_primes_and_holds_steady() {
    signals::alpha_engine<5> eng(signals::alpha_engine<5>::config{0.3, 0.05});

    const q16::fp_t c = q16::from_double(0.5);
    check(!eng.primed());
    check(approx(q16::to_double(eng.step(c)), 0.5));  // primes straight to the sample
    check(eng.primed());

    // feeding the same value again leaves the level (& velocity) unchanged.
    check(approx(q16::to_double(eng.step(c)), 0.5));
    check(approx(eng.velocity(), 0.0));
    check(approx(eng.alpha(), 0.5));
}

// a step change is tracked monotonically toward the target, staying bounded.
void filter_tracks_step_monotonically() {
    signals::alpha_engine<5> eng(signals::alpha_engine<5>::config{0.3, 0.05});

    eng.step(0);  // prime at zero
    const q16::fp_t target = q16::one;  // drive toward +1
    double prev = eng.alpha();
    for (int i = 0; i < 16; ++i) {
        const double level = q16::to_double(eng.step(target));
        check(level > prev - 1e-9);  // non-decreasing toward the target
        check(level <= 1.0 + 1e-9);  // never overshoots past the clamp
        prev = level;
    }
    check(eng.alpha() > 0.5);  // has made real progress toward +1
}

// the book_update convenience path agrees with the raw compute + step.
void on_book_matches_manual() {
    signals::alpha_engine<5> eng;

    book_update u;
    u.two_sided  = true;
    u.bid_levels = 5;
    u.ask_levels = 5;
    for (std::size_t k = 0; k < 5; ++k) {
        u.bid_sz[k] = 200;          // bid-heavy across the board
        u.ask_sz[k] = 50;
    }
    const double a = q16::to_double(eng.on_book(u));
    check(a > 0.0);                 // bid-heavy => positive alpha
    check(approx(eng.obi(), a));    // first sample => filtered == raw
}

}  // namespace

int main() {
    run_suite(obi_sign_and_bounds);
    run_suite(obi_near_levels_dominate);
    run_suite(filter_primes_and_holds_steady);
    run_suite(filter_tracks_step_monotonically);
    run_suite(on_book_matches_manual);
    return hft_test_summary("alpha_engine");
}
