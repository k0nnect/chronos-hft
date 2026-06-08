// behavioural tests for the l3 order book: best-price tracking, fifo priority,
// partial/full executions, cancels that empty a level, replace semantics, the
// analytics (spread / mid / micro-price / imbalance) & out-of-band rejection.
#include <cmath>
#include <memory>

#include "check.hpp"
#include "hft/book/order_book.hpp"

using namespace hft;

namespace {

// small book: 1024-tick band, 256 orders. base at 10000 ticks.
using book_t = order_book<1024, 256>;
constexpr price_t kBase = 10000;

bool approx(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

void empty_book_reports_no_top() {
    auto book = std::make_unique<book_t>(kBase);
    check(!book->has_bid());
    check(!book->has_ask());
    check_eq(book->best_bid(), no_price);
    check_eq(book->best_ask(), no_price);
    check_eq(book->live_orders(), 0u);
    check(approx(book->imbalance(1), 0.0));
}

void best_price_tracking_and_aggregation() {
    auto book = std::make_unique<book_t>(kBase);
    check(book->add(1, side::bid, kBase + 100, 500));
    check(book->add(2, side::bid, kBase + 100, 300));   // same level, aggregates
    check(book->add(3, side::bid, kBase + 99, 1000));   // worse bid
    check(book->add(4, side::ask, kBase + 102, 400));
    check(book->add(5, side::ask, kBase + 103, 700));   // worse ask

    check_eq(book->best_bid(), kBase + 100);
    check_eq(book->best_ask(), kBase + 102);
    check_eq(book->best_bid_qty(), 800u);               // 500 + 300
    check_eq(book->best_ask_qty(), 400u);
    check_eq(book->spread(), 2);
    check(approx(book->mid(), static_cast<double>(kBase) + 101.0));
    check_eq(book->live_orders(), 5u);

    // a new better bid moves the inside.
    check(book->add(6, side::bid, kBase + 101, 200));
    check_eq(book->best_bid(), kBase + 101);
    check_eq(book->best_bid_qty(), 200u);
}

void fifo_priority_and_partial_then_full_fill() {
    auto book = std::make_unique<book_t>(kBase);
    book->add(1, side::bid, kBase + 50, 500);  // front of queue
    book->add(2, side::bid, kBase + 50, 300);  // behind id 1
    check_eq(book->best_bid_qty(), 800u);

    // partial fill hits the resting order by id; level total drops, order stays.
    check(book->execute(1, 200));
    check_eq(book->best_bid_qty(), 600u);      // 300 left on id1 + 300 on id2
    check_eq(book->live_orders(), 2u);

    // finish off id1; id2 remains & the level survives.
    check(book->execute(1, 300));
    check_eq(book->best_bid_qty(), 300u);
    check_eq(book->live_orders(), 1u);
    check_eq(book->best_bid(), kBase + 50);

    // overfill is clamped & removes the order.
    check(book->execute(2, 99999));
    check(!book->has_bid());
    check_eq(book->live_orders(), 0u);
}

void cancel_empties_level_and_demotes_best() {
    auto book = std::make_unique<book_t>(kBase);
    book->add(1, side::ask, kBase + 10, 100);  // best ask
    book->add(2, side::ask, kBase + 11, 100);  // next level
    book->add(3, side::ask, kBase + 12, 100);
    check_eq(book->best_ask(), kBase + 10);

    // cancelling the only order at the inside should reveal the next level.
    check(book->cancel(1));
    check_eq(book->best_ask(), kBase + 11);
    check(book->cancel(2));
    check_eq(book->best_ask(), kBase + 12);
    check(book->cancel(3));
    check(!book->has_ask());

    // cancelling an unknown id is a no-op failure.
    check(!book->cancel(999));
}

void replace_forfeits_priority_and_can_move_price() {
    auto book = std::make_unique<book_t>(kBase);
    book->add(1, side::bid, kBase + 20, 100);
    book->add(2, side::bid, kBase + 20, 100);
    // replace id1 up to a new best price with new size & a new id.
    check(book->replace(1, 3, kBase + 21, 250));
    check_eq(book->best_bid(), kBase + 21);
    check_eq(book->best_bid_qty(), 250u);
    // old id is gone, old level still has id2.
    check(!book->cancel(1));
    check_eq(book->live_orders(), 2u);
}

void micro_price_and_imbalance() {
    auto book = std::make_unique<book_t>(kBase);
    book->add(1, side::bid, kBase + 100, 800);
    book->add(2, side::ask, kBase + 102, 200);

    // micro = (bid*ask_qty + ask*bid_qty) / (bid_qty + ask_qty)
    //       = (10100*200 + 10102*800) / 1000 = 10101.6
    check(approx(book->micro_price(), 10101.6));
    // imbalance(1) = (800 - 200) / 1000 = 0.6
    check(approx(book->imbalance(1), 0.6));

    // add depth & check multi-level imbalance.
    book->add(3, side::bid, kBase + 99, 200);   // bid depth now 1000 over 2 levels
    book->add(4, side::ask, kBase + 103, 800);  // ask depth now 1000 over 2 levels
    check(approx(book->imbalance(2), 0.0));     // (1000 - 1000) / 2000
}

void out_of_band_and_pool_limits_are_rejected() {
    auto book = std::make_unique<book_t>(kBase);
    // below the band.
    check(!book->add(1, side::bid, kBase - 1, 100));
    // above the band (base + NumTicks is the first invalid tick).
    check(!book->add(2, side::ask, kBase + 1024, 100));
    // in band is fine.
    check(book->add(3, side::bid, kBase + 0, 100));
    check(book->add(4, side::ask, kBase + 1023, 100));
    check_eq(book->live_orders(), 2u);
}

}  // namespace

int main() {
    run_suite(empty_book_reports_no_top);
    run_suite(best_price_tracking_and_aggregation);
    run_suite(fifo_priority_and_partial_then_full_fill);
    run_suite(cancel_empties_level_and_demotes_best);
    run_suite(replace_forfeits_priority_and_can_move_price);
    run_suite(micro_price_and_imbalance);
    run_suite(out_of_band_and_pool_limits_are_rejected);
    return hft_test_summary("order_book");
}
