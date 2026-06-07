// exercises the open-addressing order-id -> slot map: insert/find/erase, the
// duplicate-key guard, tombstone reuse and probe-chain integrity under churn.
#include "check.hpp"
#include "hft/book/order_index_map.hpp"

using namespace hft;

namespace {

void basic_insert_find_erase() {
    order_index_map<16> map;
    check_eq(map.size(), 0u);

    check(map.insert(1001, 5));
    check(map.insert(2002, 9));
    check(map.insert(3003, 11));
    check_eq(map.size(), 3u);

    check_eq(map.find(1001), 5u);
    check_eq(map.find(2002), 9u);
    check_eq(map.find(3003), 11u);
    check_eq(map.find(4004), invalid_slot);  // absent

    // duplicate insert is rejected and does not change the mapping.
    check(!map.insert(1001, 999));
    check_eq(map.find(1001), 5u);

    check_eq(map.erase(2002), 9u);
    check_eq(map.find(2002), invalid_slot);
    check_eq(map.erase(2002), invalid_slot);  // erase of absent id
    check_eq(map.size(), 2u);
}

void tombstone_reuse_keeps_probe_chain_intact() {
    // force collisions and deletions, then make sure later lookups still walk
    // past tombstones to find live entries.
    order_index_map<8> map;
    for (order_id_t id = 1; id <= 5; ++id) {
        check(map.insert(id, static_cast<slot_t>(id * 10)));
    }
    check_eq(map.erase(2), 20u);
    check_eq(map.erase(3), 30u);
    // reinsert reusing tombstone slots.
    check(map.insert(2, 21));
    check(map.insert(3, 31));
    check_eq(map.find(1), 10u);
    check_eq(map.find(2), 21u);
    check_eq(map.find(3), 31u);
    check_eq(map.find(4), 40u);
    check_eq(map.find(5), 50u);
}

void fills_to_capacity() {
    order_index_map<16> map;
    // load up to capacity; every insert must succeed and be findable.
    for (order_id_t id = 1; id <= 16; ++id) {
        check(map.insert(id, static_cast<slot_t>(id)));
    }
    check_eq(map.size(), 16u);
    // table is full: a further insert is rejected rather than looping forever.
    check(!map.insert(17, 17));
    for (order_id_t id = 1; id <= 16; ++id) {
        check_eq(map.find(id), static_cast<slot_t>(id));
    }
}

}  // namespace

int main() {
    run_suite(basic_insert_find_erase);
    run_suite(tombstone_reuse_keeps_probe_chain_intact);
    run_suite(fills_to_capacity);
    return hft_test_summary("order_index_map");
}
