// the two flat records that make up the book: a single resting order & an
// aggregated price level. both are deliberately trivial, tightly packed &
// 32-bit-handle based so a whole level's worth of bookkeeping fits in one or
// two cache lines.
#pragma once

#include "hft/core/types.hpp"

namespace hft {

// a resting order, stored by value inside the object pool. prev/next form an
// intrusive doubly linked list that threads through the orders sitting at one
// price level, preserving exchange time priority (fifo) without any per-order
// heap node. layout is hand-ordered widest-first to avoid internal padding:
// 8 + 8 + 4 + 4 + 4 + 1 -> 32 bytes (one half cache line) after natural padding.
struct order {
    qty_t       qty;        // remaining open quantity
    order_id_t  id;         // exchange order reference
    level_idx_t price_idx;  // index into this side's level array
    slot_t      prev;       // previous order at this level, or invalid_slot
    slot_t      next;       // next order at this level, or invalid_slot
    side        s;          // resting side
};

// an aggregated price level: the running totals plus the head/tail of the fifo
// queue of resting orders. queries that only need depth (imbalance, micro-price)
// read total_qty directly & never walk the order list.
struct price_level {
    qty_t         total_qty   = 0;             // sum of open qty across orders
    std::uint32_t order_count = 0;             // number of resting orders
    slot_t        head        = invalid_slot;  // oldest order (front of fifo)
    slot_t        tail        = invalid_slot;  // newest order (back of fifo)

    [[nodiscard]] constexpr bool empty() const noexcept { return order_count == 0; }
};

}  // namespace hft
