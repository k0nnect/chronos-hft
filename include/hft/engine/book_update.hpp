// a top-of-book snapshot handed to the strategy after each event is applied.
//
// it is computed once per tick by the engine straight from the book's cached
// aggregates (no scanning), so the strategy gets fair-value inputs -- mid,
// micro-price, imbalance -- without recomputing them. trivially copyable so it
// can also be queued if a strategy ever runs on its own thread.
#pragma once

#include <cstdint>

#include "hft/core/types.hpp"

namespace hft {

struct book_update {
    ts_t          timestamp   = 0;
    price_t       best_bid    = no_price;  // no_price when the bid side is empty
    price_t       best_ask    = no_price;  // no_price when the ask side is empty
    qty_t         bid_qty     = 0;         // resting size at the inside bid
    qty_t         ask_qty     = 0;         // resting size at the inside ask
    double        mid         = 0.0;       // (bid+ask)/2 in ticks
    double        micro_price = 0.0;       // size-weighted fair value in ticks
    double        imbalance   = 0.0;       // top-level order-flow imbalance [-1,1]
    bool          two_sided   = false;     // true only when both sides are present
};

}  // namespace hft
