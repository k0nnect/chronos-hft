// the normalized, host-endian market event the rest of the system consumes.
//
// the feed handler decodes raw wire messages into this single flat record so the
// engine never sees protocol details. it is a trivially-copyable pod sized to sit
// in one cache line, which is exactly what the spsc ring needs to move it between
// the feed thread and the engine thread with a single aligned store/load.
#pragma once

#include <cstdint>

#include "hft/core/types.hpp"

namespace hft {

enum class event_type : std::uint8_t {
    add = 0,       // new resting order
    execute,       // resting order filled (by id), qty = executed shares
    reduce,        // resting order partially cancelled, qty = cancelled shares
    delete_order,  // resting order fully cancelled / removed
    replace,       // cancel original_id, add new_id at new price/qty
    trade,         // non-book trade print (hidden/odd-lot), informational
};

[[nodiscard]] constexpr const char* to_string(event_type t) noexcept {
    switch (t) {
        case event_type::add:          return "add";
        case event_type::execute:      return "execute";
        case event_type::reduce:       return "reduce";
        case event_type::delete_order: return "delete";
        case event_type::replace:      return "replace";
        case event_type::trade:        return "trade";
    }
    return "?";
}

// fields are ordered widest-first so the struct packs with no internal padding.
// unused fields for a given event type are left zeroed by the decoder.
struct market_event {
    ts_t          timestamp    = 0;  // exchange timestamp, nanoseconds
    order_id_t    order_id     = 0;  // primary order reference
    order_id_t    new_order_id = 0;  // replace only: the replacement reference
    qty_t         qty          = 0;  // shares (added / executed / cancelled)
    price_t       price        = 0;  // price in ticks (add / replace / trade)
    std::uint64_t match_number = 0;  // execution / trade match id
    std::uint16_t stock_locate = 0;  // instrument id
    event_type    type         = event_type::add;
    side          s            = side::bid;
};

}  // namespace hft
