// phase 1 demo + micro-benchmark driver for the software order book.
//
// builds a book, replays a small hand-built sequence of add / execute / cancel
// messages, prints the resulting top of book and analytics, then hammers the
// hot path with a deterministic synthetic workload and reports nanoseconds per
// operation. no real feed yet -- that is phase 2.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

#include "hft/book/order_book.hpp"

namespace {

// a tight band is plenty for a single instrument: 1<<16 ticks ~= 65k price
// points, and a million resting orders. both arrays are heap-reserved once.
using book_t = hft::order_book</*NumTicks=*/1u << 16, /*MaxOrders=*/1u << 20>;

constexpr hft::price_t kBaseTick = 1'000'000;  // arbitrary tick origin

void print_top(const book_t& book) {
    if (book.has_bid() && book.has_ask()) {
        std::printf(
            "  bid %lld x%llu | ask %lld x%llu | spread %lld | mid %.2f | micro %.4f | "
            "imb(1) %+.4f | imb(5) %+.4f | live %zu\n",
            static_cast<long long>(book.best_bid()),
            static_cast<unsigned long long>(book.best_bid_qty()),
            static_cast<long long>(book.best_ask()),
            static_cast<unsigned long long>(book.best_ask_qty()),
            static_cast<long long>(book.spread()), book.mid(), book.micro_price(),
            book.imbalance(1), book.imbalance(5), book.live_orders());
    } else {
        std::printf("  (one side empty) live %zu\n", book.live_orders());
    }
}

void functional_demo() {
    std::puts("== functional demo ==");
    auto book = std::make_unique<book_t>(kBaseTick);

    // build a small two-sided book. prices are absolute ticks.
    book->add(/*id=*/1, hft::side::bid, kBaseTick + 100, 500);
    book->add(/*id=*/2, hft::side::bid, kBaseTick + 100, 300);  // joins the queue behind id 1
    book->add(/*id=*/3, hft::side::bid, kBaseTick + 99, 1000);
    book->add(/*id=*/4, hft::side::ask, kBaseTick + 102, 400);
    book->add(/*id=*/5, hft::side::ask, kBaseTick + 103, 700);
    book->add(/*id=*/6, hft::side::ask, kBaseTick + 102, 250);
    std::puts("after initial adds:");
    print_top(*book);

    // partial fill of the front bid; it keeps time priority.
    book->execute(/*id=*/1, 200);
    std::puts("after partial fill 200 @ best bid:");
    print_top(*book);

    // cancel the rest of the best ask order, lifting the inside ask qty.
    book->cancel(/*id=*/4);
    std::puts("after cancel of one best-ask order:");
    print_top(*book);

    // replace pushes a bid through the spread to a new best price (loses priority).
    book->replace(/*old_id=*/3, /*new_id=*/7, kBaseTick + 101, 800);
    std::puts("after replace lifting the bid to a new best:");
    print_top(*book);

    // drain the whole best ask level via a full execution.
    book->execute(/*id=*/6, 250);
    std::puts("after full fill of remaining best-ask order:");
    print_top(*book);
}

void benchmark() {
    std::puts("\n== hot-path micro-benchmark ==");
    auto book = std::make_unique<book_t>(kBaseTick);

    constexpr std::size_t kOrders = 1u << 20;       // ~1.05m orders
    constexpr std::size_t kBand   = 4096;           // price spread around the seed

    // pre-generate a deterministic workload so timing measures the book, not rng.
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<std::int64_t> price_off(0, kBand - 1);
    std::uniform_int_distribution<std::uint64_t> qty_dist(1, 1000);

    struct msg {
        hft::order_id_t id;
        hft::price_t    price;
        hft::qty_t      qty;
        hft::side       s;
    };
    std::vector<msg> msgs;
    msgs.reserve(kOrders);
    for (std::size_t i = 0; i < kOrders; ++i) {
        const hft::side s = (i & 1) ? hft::side::ask : hft::side::bid;
        const hft::price_t base = (s == hft::side::bid) ? kBaseTick + 2048 : kBaseTick + 2048 + 1;
        const hft::price_t px = (s == hft::side::bid) ? base - price_off(rng)
                                                      : base + price_off(rng);
        msgs.push_back({static_cast<hft::order_id_t>(i + 1), px, qty_dist(rng), s});
    }

    // time the adds.
    auto t0 = std::chrono::steady_clock::now();
    for (const msg& m : msgs) {
        book->add(m.id, m.s, m.price, m.qty);
    }
    auto t1 = std::chrono::steady_clock::now();

    // touch top of book so the optimiser cannot elide the loop above.
    volatile double sink = book->micro_price() + book->imbalance(5);
    (void)sink;

    // time the cancels (same ids, reverse order).
    auto t2 = std::chrono::steady_clock::now();
    for (std::size_t i = msgs.size(); i-- > 0;) {
        book->cancel(msgs[i].id);
    }
    auto t3 = std::chrono::steady_clock::now();

    const double add_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kOrders;
    const double cxl_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / kOrders;
    std::printf("  add:    %8.2f ns/op  (%zu ops)\n", add_ns, kOrders);
    std::printf("  cancel: %8.2f ns/op  (%zu ops)\n", cxl_ns, kOrders);
    std::printf("  residual live orders after full cancel: %zu (expect 0)\n",
                book->live_orders());
}

}  // namespace

int main() {
    functional_demo();
    benchmark();
    return 0;
}
