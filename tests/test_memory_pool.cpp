// exercises the object pool: acquisition order, exhaustion, & reuse on release.
#include "check.hpp"
#include "hft/core/memory_pool.hpp"

using namespace hft;

namespace {

void acquire_release_cycle() {
    object_pool<int, 4> pool;
    check_eq(pool.capacity(), 4u);
    check_eq(pool.in_use(), 0u);
    check_eq(pool.available(), 4u);

    // first acquisitions hand out 0,1,2,3 in order.
    const slot_t a = pool.acquire();
    const slot_t b = pool.acquire();
    const slot_t c = pool.acquire();
    const slot_t d = pool.acquire();
    check_eq(a, 0u);
    check_eq(b, 1u);
    check_eq(c, 2u);
    check_eq(d, 3u);
    check_eq(pool.in_use(), 4u);

    // pool is now empty: next acquire reports failure rather than overrunning.
    check_eq(pool.acquire(), invalid_slot);

    // values stored through the handle round-trip.
    pool[a] = 42;
    pool[b] = 7;
    check_eq(pool[a], 42);
    check_eq(pool[b], 7);

    // releasing returns the slot to the free list for reuse.
    pool.release(b);
    check_eq(pool.in_use(), 3u);
    const slot_t reused = pool.acquire();
    check_eq(reused, b);
    check_eq(pool.in_use(), 4u);
}

void reset_restores_capacity() {
    object_pool<long, 8> pool;
    for (int i = 0; i < 8; ++i) (void)pool.acquire();
    check_eq(pool.in_use(), 8u);
    pool.reset();
    check_eq(pool.in_use(), 0u);
    check_eq(pool.acquire(), 0u);  // reset re-orders so slot 0 comes first again
}

}  // namespace

int main() {
    run_suite(acquire_release_cycle);
    run_suite(reset_restores_capacity);
    return hft_test_summary("memory_pool");
}
