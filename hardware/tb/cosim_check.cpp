// co-simulation cross-check: synthetic feed -> { verilated rtl, software
// reference } & assert they agree.
//
// for every two-sided book state produced by replaying the phase-2 generator we
// compute three things:
//   1. the rtl output (from the verilated feature_extractor, via FpgaFeatureEngine)
//   2. the bit-exact software model (feature_reference)
//   3. the floating-point order_book reference (micro_price / imbalance)
// & require (1) == (2) exactly (bit-for-bit) & |(2) - (3)| within a tight ulp
// window. the rtl is pipelined, so outputs are collected in order & matched to
// the inputs that produced them.
//
// exit code is non-zero on any mismatch so it can run as a ctest case.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>

#include "hardware/dpi/feature_reference.hpp"
#include "hardware/dpi/fpga_dpi.hpp"
#include "hft/book/apply.hpp"
#include "hft/book/order_book.hpp"
#include "hft/feed/itch_generator.hpp"
#include "hft/feed/itch_parser.hpp"
#include "hft/feed/market_event.hpp"

using namespace hft;

namespace {

using book_t = order_book<8192, 1u << 20>;

// q16.16 of the golden vs the float reference may differ by a few lsb because the
// hardware truncates where the float path rounds. this is the "ulp window".
constexpr long kMicroTol = 8;
constexpr long kImbTol   = 8;

struct expected {
    hw::feature_fixed golden;       // bit-exact software model (== rtl)
    long              ref_micro;    // round(float micro_rel * 2^16)
    long              ref_imb;      // round(float imbalance * 2^16)
};

}  // namespace

int main() {
    constexpr std::size_t kMessages = 500'000;
    const itch::synthetic_feed feed = itch::generate_feed(kMessages, /*seed=*/4242);

    auto book = std::make_unique<book_t>(feed.base_tick);
    hw::FpgaFeatureEngine fpga(feed.base_tick);
    fpga.reset();

    std::deque<expected> inflight;

    std::uint64_t pushed = 0, popped = 0, exact_mismatch = 0, ulp_violation = 0;
    long          worst_micro = 0, worst_imb = 0;

    auto check_output = [&](std::uint32_t hw_micro, std::int32_t hw_imb) {
        const expected e = inflight.front();
        inflight.pop_front();
        ++popped;

        if (hw_micro != e.golden.micro_price || hw_imb != e.golden.imbalance) {
            ++exact_mismatch;
        }
        const long dmi = std::labs(static_cast<long>(e.golden.micro_price) - e.ref_micro);
        const long dim = std::labs(static_cast<long>(e.golden.imbalance) - e.ref_imb);
        if (dmi > worst_micro) worst_micro = dmi;
        if (dim > worst_imb) worst_imb = dim;
        if (dmi > kMicroTol || dim > kImbTol) ++ulp_violation;
    };

    itch::frame_cursor  cursor(feed.bytes.data(), feed.bytes.size());
    std::uint16_t       len = 0;
    market_event        ev;
    for (const std::uint8_t* body = cursor.next(len); body != nullptr;
         body = cursor.next(len)) {
        if (!itch::decode(body, len, ev)) continue;
        apply_event(*book, ev);

        const bool two_sided = book->has_bid() && book->has_ask() &&
                               book->best_bid_qty() > 0 && book->best_ask_qty() > 0;
        if (two_sided) {
            const price_t bb = book->best_bid();
            const price_t ba = book->best_ask();
            const qty_t   bq = book->best_bid_qty();
            const qty_t   aq = book->best_ask_qty();

            book_update u;
            u.timestamp   = ev.timestamp;
            u.two_sided   = true;
            u.best_bid    = bb;
            u.best_ask    = ba;
            u.bid_qty     = bq;
            u.ask_qty     = aq;
            u.micro_price = book->micro_price();
            u.imbalance   = book->imbalance(1);

            expected e;
            e.golden = hw::compute_features_fixed(
                static_cast<std::uint32_t>(bb - feed.base_tick),
                static_cast<std::uint32_t>(ba - feed.base_tick),
                static_cast<std::uint32_t>(bq), static_cast<std::uint32_t>(aq));
            e.ref_micro = std::lround((u.micro_price - static_cast<double>(feed.base_tick)) *
                                      65536.0);
            e.ref_imb   = std::lround(u.imbalance * 65536.0);
            inflight.push_back(e);

            fpga.push_book_tick(u);
            ++pushed;
        } else {
            fpga.idle_tick();
        }

        std::uint32_t hw_micro;
        std::int32_t  hw_imb;
        if (fpga.pop_feature_tick(hw_micro, hw_imb)) {
            check_output(hw_micro, hw_imb);
        }
    }

    // flush the pipeline.
    while (popped < pushed) {
        fpga.idle_tick();
        std::uint32_t hw_micro;
        std::int32_t  hw_imb;
        if (fpga.pop_feature_tick(hw_micro, hw_imb)) {
            check_output(hw_micro, hw_imb);
        }
    }

    std::printf("== feature_extractor cosimulation ==\n");
    std::printf("  features checked      : %llu\n", static_cast<unsigned long long>(popped));
    std::printf("  pipeline latency      : %u cycles\n", hw::FpgaFeatureEngine::latency_cycles());
    std::printf("  rtl vs golden (exact) : %llu mismatch(es)\n",
                static_cast<unsigned long long>(exact_mismatch));
    std::printf("  golden vs float ulp   : micro<=%ld imb<=%ld (tol %ld/%ld), %llu violation(s)\n",
                worst_micro, worst_imb, kMicroTol, kImbTol,
                static_cast<unsigned long long>(ulp_violation));

    const bool ok = (exact_mismatch == 0) && (ulp_violation == 0) && (popped == pushed) &&
                    (popped > 0);
    std::printf("  result                : %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
