// glue that applies a normalized market_event to an order_book.
//
// this is the minimal mapping from feed semantics to book mutations -- the full
// strategy-aware engine arrives in phase 3. both an execute ('E') & a partial
// cancel ('X') reduce a resting order's quantity, so both map onto the book's
// execute(), which reduces & removes on exhaustion. trade prints do not touch
// the displayed book. returns false if the underlying book operation was rejected
// (unknown id, out-of-band price, pool exhausted).
#pragma once

#include "hft/core/compiler.hpp"
#include "hft/feed/market_event.hpp"

namespace hft {

template <typename Book>
hft_hot inline bool apply_event(Book& book, const market_event& ev) noexcept {
    switch (ev.type) {
        case event_type::add:
            return book.add(ev.order_id, ev.s, ev.price, ev.qty);
        case event_type::execute:
        case event_type::reduce:
            return book.execute(ev.order_id, ev.qty);
        case event_type::delete_order:
            return book.cancel(ev.order_id);
        case event_type::replace:
            return book.replace(ev.order_id, ev.new_order_id, ev.price, ev.qty);
        case event_type::trade:
            return true;  // informational; no book change
    }
    return false;
}

}  // namespace hft
