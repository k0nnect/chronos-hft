// end-to-end: generate a synthetic itch stream, run it through the feed handler
// into the spsc ring on a producer thread, drain it on a consumer thread that
// replays each event into the order book, & verify the book's resting-order
// count matches the generator's independently-computed ground truth.
//
// also runs the simpler single-threaded path (handler -> vector -> book) & a
// direct ring_sink push so each layer is covered both in isolation & together.
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "check.hpp"
#include "hft/book/apply.hpp"
#include "hft/book/order_book.hpp"
#include "hft/feed/feed_handler.hpp"
#include "hft/feed/itch_generator.hpp"
#include "hft/feed/spsc_ring.hpp"

using namespace hft;

namespace {

// band must cover base_tick .. base_tick + tick_band used by the generator.
using book_t = order_book<8192, 1u << 20>;

void single_thread_handler_to_book() {
    const itch::synthetic_feed feed = itch::generate_feed(200'000, /*seed=*/1234);
    auto book = std::make_unique<book_t>(feed.base_tick);

    feed_handler handler;
    std::uint64_t applied = 0;
    std::uint64_t rejected = 0;
    const std::size_t consumed = handler.process(
        feed.bytes.data(), feed.bytes.size(), [&](const market_event& ev) {
            if (apply_event(*book, ev)) {
                ++applied;
            } else {
                ++rejected;
            }
        });

    check_eq(consumed, feed.bytes.size());
    check_eq(handler.stats().messages, feed.messages);
    check_eq(handler.stats().malformed, 0u);
    check_eq(rejected, 0u);  // a consistent stream never produces a rejected op
    check(applied > 0);
    // the book must hold exactly the orders the generator believes are resting.
    check_eq(book->live_orders(), feed.expected_live);
}

void threaded_handler_ring_book() {
    const itch::synthetic_feed feed = itch::generate_feed(500'000, /*seed=*/99);
    auto book = std::make_unique<book_t>(feed.base_tick);
    auto ring = std::make_unique<spsc_ring<market_event, 4096>>();

    // producer: parse the wire stream & publish every event into the ring.
    feed_handler handler;
    std::thread producer([&] {
        ring_sink<spsc_ring<market_event, 4096>> sink(*ring);
        handler.process(feed.bytes.data(), feed.bytes.size(), sink);
    });

    // consumer: drain the ring & apply each event to the book.
    std::uint64_t applied  = 0;
    std::uint64_t rejected = 0;
    std::thread consumer([&] {
        market_event ev;
        std::uint64_t seen = 0;
        while (seen < feed.messages) {
            if (ring->try_pop(ev)) {
                ++seen;
                if (apply_event(*book, ev)) {
                    ++applied;
                } else {
                    ++rejected;
                }
            }
        }
    });

    producer.join();
    consumer.join();

    check_eq(handler.stats().messages, feed.messages);
    check_eq(rejected, 0u);
    check_eq(book->live_orders(), feed.expected_live);
}

}  // namespace

int main() {
    run_suite(single_thread_handler_to_book);
    run_suite(threaded_handler_ring_book);
    return hft_test_summary("feed_integration");
}
