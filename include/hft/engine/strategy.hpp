// zero-overhead strategy interface.
//
// strategies derive from strategy<Derived> (curiously recurring template
// pattern), so the engine dispatches to callbacks through a static_cast instead
// of a vtable -- no indirect call, fully inlinable. derived classes implement
// any of on_market_event / on_book_update / on_order_fill; the base supplies
// empty defaults so a strategy only writes the hooks it cares about.
//
// orders flow back through `order_gateway`, a concrete (non-virtual) object the
// engine binds into the strategy. submitting an order just appends an intent to
// a fixed buffer; the engine drains that buffer once per tick & registers the
// intents with the fill model using the current book & timestamp. this keeps
// the strategy free of any knowledge of the book/fill-model types & keeps the
// submission path allocation-free & branch-light.
#pragma once

#include <array>
#include <cstddef>

#include "hft/core/compiler.hpp"
#include "hft/core/types.hpp"
#include "hft/engine/book_update.hpp"
#include "hft/engine/fill_model.hpp"
#include "hft/feed/market_event.hpp"

namespace hft {

enum class order_action : std::uint8_t { limit, market, cancel };

struct order_intent {
    order_action action;
    side         s;
    price_t      price;  // unused for market / cancel
    qty_t        qty;    // unused for cancel
    order_id_t   id;     // assigned id (submit) or target id (cancel)
};

// fixed-capacity, single-threaded staging buffer for the orders a strategy emits
// while handling one event. the engine owns it, binds it to the strategy, drains
// it after each tick, & clears it.
template <std::size_t Capacity = 512>
class basic_order_gateway {
    static_assert(Capacity > 0, "gateway needs capacity");

public:
    [[nodiscard]] hft_always_inline order_id_t submit_limit(side s, price_t price,
                                                            qty_t qty) noexcept {
        const order_id_t id = next_id_++;
        push(order_intent{order_action::limit, s, price, qty, id});
        return id;
    }

    [[nodiscard]] hft_always_inline order_id_t submit_market(side s, qty_t qty) noexcept {
        const order_id_t id = next_id_++;
        push(order_intent{order_action::market, s, 0, qty, id});
        return id;
    }

    hft_always_inline bool cancel(order_id_t id) noexcept {
        push(order_intent{order_action::cancel, side::bid, 0, 0, id});
        return true;
    }

    // engine-side drain interface.
    [[nodiscard]] std::size_t count() const noexcept { return n_; }
    [[nodiscard]] const order_intent& operator[](std::size_t i) const noexcept {
        return buf_[i];
    }
    void clear() noexcept { n_ = 0; }

private:
    hft_always_inline void push(const order_intent& it) noexcept {
        if (n_ >= Capacity) [[unlikely]] {
            return;  // refuse to overrun the staging buffer
        }
        buf_[n_++] = it;
    }

    std::array<order_intent, Capacity> buf_{};
    std::size_t                        n_       = 0;
    order_id_t                         next_id_ = 1;  // client ids start at 1
};

using order_gateway = basic_order_gateway<>;

template <typename Derived>
class strategy {
public:
    // bound once by the engine before any callback fires.
    void bind(order_gateway& gw) noexcept { gw_ = &gw; }

    // engine-facing dispatchers: static (no vtable) & force-inlined into the
    // engine's hot loop.
    hft_always_inline void handle_market_event(const market_event& ev) noexcept {
        derived().on_market_event(ev);
    }
    hft_always_inline void handle_book_update(const book_update& u) noexcept {
        derived().on_book_update(u);
    }
    hft_always_inline void handle_order_fill(const fill_event& f) noexcept {
        derived().on_order_fill(f);
    }

protected:
    // default no-op hooks; a derived strategy overrides only what it needs.
    void on_market_event(const market_event&) noexcept {}
    void on_book_update(const book_update&) noexcept {}
    void on_order_fill(const fill_event&) noexcept {}

    // order-entry helpers available to derived strategies.
    [[nodiscard]] hft_always_inline order_id_t submit_limit(side s, price_t price,
                                                            qty_t qty) noexcept {
        return gw_->submit_limit(s, price, qty);
    }
    [[nodiscard]] hft_always_inline order_id_t submit_market(side s, qty_t qty) noexcept {
        return gw_->submit_market(s, qty);
    }
    hft_always_inline bool cancel(order_id_t id) noexcept { return gw_->cancel(id); }

private:
    [[nodiscard]] hft_always_inline Derived& derived() noexcept {
        return *static_cast<Derived*>(this);
    }

    order_gateway* gw_ = nullptr;
};

}  // namespace hft
