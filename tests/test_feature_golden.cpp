// validates the fixed-point feature reference (the bit-exact model the rtl
// mirrors) against the floating-point order_book implementation, over a replayed
// synthetic feed. this exercises the exact arithmetic the verilated cosimulation
// asserts against, but needs no verilator toolchain, so the numeric design is
// covered by the normal test suite.
#include <cmath>
#include <cstdint>
#include <memory>

#include "check.hpp"
#include "hardware/dpi/feature_reference.hpp"
#include "hft/book/apply.hpp"
#include "hft/book/order_book.hpp"
#include "hft/feed/itch_generator.hpp"
#include "hft/feed/itch_parser.hpp"
#include "hft/feed/market_event.hpp"

using namespace hft;

namespace {

using book_t = order_book<8192, 1u << 20>;

// directed checks on the fixed-point formula at the boundaries.
void formula_edge_cases() {
    // balanced book: weight = 1/2, micro = midpoint; imbalance = 0.
    {
        const auto f = hw::compute_features_fixed(/*bid=*/100, /*ask=*/104, /*bq=*/50, /*aq=*/50);
        // micro = bid + (ask-bid)*0.5 = 102.0 -> Q16.16
        check_eq(f.micro_price, static_cast<std::uint32_t>(102u << 16));
        check_eq(f.imbalance, 0);
    }
    // all bid size: weight -> 1, micro -> ask; imbalance -> +1.
    {
        const auto f = hw::compute_features_fixed(100, 104, /*bq=*/1000, /*aq=*/1);
        // weight = floor(1000*65536/1001) = 65470; micro = (100<<16)+4*65470
        const std::uint64_t w = (1000ull << 16) / 1001ull;
        check_eq(f.micro_price, static_cast<std::uint32_t>((100ull << 16) + 4ull * w));
        // imbalance = floor(999*65536/1001) > 0, close to +1.
        check(f.imbalance > 0);
        check(f.imbalance < (1 << 16));
    }
    // all ask size: imbalance negative.
    {
        const auto f = hw::compute_features_fixed(100, 104, /*bq=*/1, /*aq=*/1000);
        check(f.imbalance < 0);
        check(f.imbalance > -(1 << 16));
    }
}

// the fixed-point reference must track the floating-point book within a tight
// ulp window across a whole replayed feed.
void matches_order_book_over_feed() {
    const itch::synthetic_feed feed = itch::generate_feed(200'000, /*seed=*/55);
    auto book = std::make_unique<book_t>(feed.base_tick);

    constexpr long kMicroTol = 8;  // lsb of Q16.16
    constexpr long kImbTol   = 8;
    long worst_micro = 0, worst_imb = 0;
    std::uint64_t checked = 0;

    itch::frame_cursor  cursor(feed.bytes.data(), feed.bytes.size());
    std::uint16_t       len = 0;
    market_event        ev;
    for (const std::uint8_t* body = cursor.next(len); body != nullptr;
         body = cursor.next(len)) {
        if (!itch::decode(body, len, ev)) continue;
        apply_event(*book, ev);

        if (!(book->has_bid() && book->has_ask())) continue;
        if (book->best_bid_qty() == 0 || book->best_ask_qty() == 0) continue;

        const price_t bb = book->best_bid();
        const price_t ba = book->best_ask();

        const auto golden = hw::compute_features_fixed(
            static_cast<std::uint32_t>(bb - feed.base_tick),
            static_cast<std::uint32_t>(ba - feed.base_tick),
            static_cast<std::uint32_t>(book->best_bid_qty()),
            static_cast<std::uint32_t>(book->best_ask_qty()));

        const long ref_micro =
            std::lround((book->micro_price() - static_cast<double>(feed.base_tick)) * 65536.0);
        const long ref_imb = std::lround(book->imbalance(1) * 65536.0);

        const long dmi = std::labs(static_cast<long>(golden.micro_price) - ref_micro);
        const long dim = std::labs(static_cast<long>(golden.imbalance) - ref_imb);
        if (dmi > worst_micro) worst_micro = dmi;
        if (dim > worst_imb) worst_imb = dim;
        check(dmi <= kMicroTol);
        check(dim <= kImbTol);
        ++checked;
    }

    std::printf("  checked %llu two-sided states; worst ulp micro=%ld imb=%ld\n",
                static_cast<unsigned long long>(checked), worst_micro, worst_imb);
    check(checked > 0);
}

}  // namespace

int main() {
    run_suite(formula_edge_cases);
    run_suite(matches_order_book_over_feed);
    return hft_test_summary("feature_golden");
}
