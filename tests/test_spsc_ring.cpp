// exercises the spsc ring: empty/full edges, fifo order, wrap-around, & a
// real two-thread producer/consumer run that checks every item arrives exactly
// once & in order.
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "check.hpp"
#include "hft/feed/spsc_ring.hpp"

using namespace hft;

namespace {

void empty_and_full_edges() {
    spsc_ring<int, 4> ring;  // capacity 4
    check_eq(ring.capacity(), 4u);
    int out = -1;
    check(!ring.try_pop(out));  // empty

    check(ring.try_push(10));
    check(ring.try_push(20));
    check(ring.try_push(30));
    check(ring.try_push(40));   // now full
    check(!ring.try_push(50));  // rejected, no overwrite
    check_eq(ring.size_approx(), 4u);

    check(ring.try_pop(out));
    check_eq(out, 10);  // fifo
    check(ring.try_push(50));  // space again
    check(!ring.try_push(60));
}

void fifo_order_and_wraparound() {
    spsc_ring<std::uint64_t, 8> ring;
    // push/pop many times so head/tail run well past capacity & wrap the mask.
    std::uint64_t next_push = 0;
    std::uint64_t next_pop  = 0;
    for (int round = 0; round < 1000; ++round) {
        // push a few
        for (int k = 0; k < 5; ++k) {
            if (ring.try_push(next_push)) ++next_push;
        }
        // pop a few, checking strict fifo
        std::uint64_t out = 0;
        for (int k = 0; k < 3; ++k) {
            if (ring.try_pop(out)) {
                check_eq(out, next_pop);
                ++next_pop;
            }
        }
    }
    // drain the remainder, still in order
    std::uint64_t out = 0;
    while (ring.try_pop(out)) {
        check_eq(out, next_pop);
        ++next_pop;
    }
    check_eq(next_pop, next_push);
}

void threaded_producer_consumer() {
    constexpr std::uint64_t kCount = 2'000'000;
    auto ring = std::make_unique<spsc_ring<std::uint64_t, 1024>>();

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!ring->try_push(i)) {
                // spin until the consumer drains a slot
            }
        }
    });

    std::uint64_t expected = 0;
    std::uint64_t received = 0;
    bool          ordered  = true;
    std::thread consumer([&] {
        std::uint64_t out = 0;
        while (received < kCount) {
            if (ring->try_pop(out)) {
                if (out != expected) ordered = false;
                ++expected;
                ++received;
            }
        }
    });

    producer.join();
    consumer.join();
    check(ordered);
    check_eq(received, kCount);
}

}  // namespace

int main() {
    run_suite(empty_and_full_edges);
    run_suite(fifo_order_and_wraparound);
    run_suite(threaded_producer_consumer);
    return hft_test_summary("spsc_ring");
}
