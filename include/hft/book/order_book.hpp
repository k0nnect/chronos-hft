// cache-friendly l3 limit order book.
//
// design
// ------
// * prices are integer ticks. each side owns a flat, directly-indexed array of
//   price_level records covering a fixed band [base_tick, base_tick + NumTicks).
//   tick -> level is a single subtraction & a bounds check, so locating a
//   level is O(1) with no hashing & no tree walk.
// * resting orders live in a single object_pool & are threaded into per-level
//   fifo queues via intrusive prev/next indices, preserving exchange time
//   priority. add / cancel / execute are all O(1).
// * order id -> pool slot goes through an open-addressing flat map (see
//   order_index_map) so cancels & executes, which arrive by id, are O(1) too.
// * best-bid / best-ask are cached as integer indices & only re-scanned when
//   the current best level empties. the scan walks the contiguous level array,
//   which is the cheapest possible memory access pattern.
//
// every allocation happens in the constructor. nothing on add/cancel/execute
// touches the heap, takes a lock, or throws.
#pragma once

#include <cstddef>
#include <limits>
#include <memory>

#include "hft/book/order_index_map.hpp"
#include "hft/book/price_level.hpp"
#include "hft/core/cache.hpp"
#include "hft/core/compiler.hpp"
#include "hft/core/memory_pool.hpp"
#include "hft/core/types.hpp"

namespace hft {

// rounds Capacity up to the next power of two at compile time, with headroom so
// the order-id map keeps a healthy load factor (< ~0.7) even when full of orders.
namespace detail {
constexpr std::size_t next_pow2_index_capacity(std::size_t n) {
    std::size_t target = n + (n >> 1) + 1;  // ~1.5x for load factor headroom
    std::size_t p      = 2;
    while (p < target) p <<= 1;
    return p;
}
}  // namespace detail

template <std::size_t NumTicks, std::size_t MaxOrders,
          std::size_t IndexCapacity = detail::next_pow2_index_capacity(MaxOrders)>
class order_book {
    static_assert(NumTicks > 0, "price band must be non-empty");
    static_assert(MaxOrders > 0, "order capacity must be non-zero");

public:
    // base_tick is the lowest price (in ticks) the band can represent. choose it
    // & NumTicks so the instrument's traded range sits comfortably inside.
    explicit order_book(price_t base_tick)
        : base_tick_(base_tick),
          bid_levels_(std::make_unique<price_level[]>(NumTicks)),
          ask_levels_(std::make_unique<price_level[]>(NumTicks)) {}

    order_book(const order_book&)            = delete;
    order_book& operator=(const order_book&) = delete;

    // ---- mutating operations (the critical path) --------------------------

    // insert a new resting order at the back of its price level's fifo queue.
    // returns false (without side effects) if the price is outside the band, the
    // pool is exhausted, or the id is already live.
    hft_hot bool add(order_id_t id, side s, price_t price, qty_t qty) noexcept {
        const std::size_t idx = tick_index(price);
        if (idx >= NumTicks) [[unlikely]] {
            return false;
        }
        const slot_t slot = pool_.acquire();
        if (slot == invalid_slot) [[unlikely]] {
            return false;
        }
        if (!index_.insert(id, slot)) [[unlikely]] {
            pool_.release(slot);
            return false;
        }

        order& o    = pool_[slot];
        o.qty       = qty;
        o.id        = id;
        o.price_idx = static_cast<level_idx_t>(idx);
        o.next      = invalid_slot;
        o.s         = s;

        price_level& lvl = level_array(s)[idx];
        o.prev           = lvl.tail;
        if (lvl.tail == invalid_slot) {
            lvl.head = slot;  // first order at this level
        } else {
            pool_[lvl.tail].next = slot;
        }
        lvl.tail = slot;
        lvl.total_qty += qty;
        ++lvl.order_count;

        promote_best(s, idx);
        return true;
    }

    // fully remove a resting order by id. returns false if the id is unknown.
    hft_hot bool cancel(order_id_t id) noexcept {
        const slot_t slot = index_.find(id);
        if (slot == invalid_slot) [[unlikely]] {
            return false;
        }
        remove_slot(slot);
        (void)index_.erase(id);
        return true;
    }

    // apply an execution against a resting order (an itch-style execute message
    // carries the order id & the executed quantity). partial fills keep the
    // order at the front of its queue; a full fill removes it. returns false if
    // the id is unknown. exec_qty is clamped to the remaining open quantity.
    hft_hot bool execute(order_id_t id, qty_t exec_qty) noexcept {
        const slot_t slot = index_.find(id);
        if (slot == invalid_slot) [[unlikely]] {
            return false;
        }
        order& o = pool_[slot];
        if (exec_qty >= o.qty) [[unlikely]] {
            remove_slot(slot);
            (void)index_.erase(id);
            return true;
        }
        o.qty -= exec_qty;
        level_array(o.s)[o.price_idx].total_qty -= exec_qty;
        return true;
    }

    // cancel/replace: an order id can change price &/or quantity. modelled as
    // remove-then-insert because any price change forfeits time priority, which
    // is exactly what real exchanges do. returns false if old_id is unknown or
    // the replacement cannot be placed (in which case the old order is gone, as
    // it would be on a real venue once the cancel half is acknowledged).
    hft_hot bool replace(order_id_t old_id, order_id_t new_id, price_t new_price,
                         qty_t new_qty) noexcept {
        const slot_t slot = index_.find(old_id);
        if (slot == invalid_slot) [[unlikely]] {
            return false;
        }
        const side s = pool_[slot].s;
        remove_slot(slot);
        (void)index_.erase(old_id);
        return add(new_id, s, new_price, new_qty);
    }

    void clear() noexcept {
        for (std::size_t i = 0; i < NumTicks; ++i) {
            bid_levels_[i] = price_level{};
            ask_levels_[i] = price_level{};
        }
        pool_.reset();
        index_.clear();
        best_bid_idx_ = -1;
        best_ask_idx_ = -1;
    }

    // ---- top-of-book & analytics (read only) ----------------------------

    [[nodiscard]] bool has_bid() const noexcept { return best_bid_idx_ >= 0; }
    [[nodiscard]] bool has_ask() const noexcept { return best_ask_idx_ >= 0; }

    [[nodiscard]] price_t best_bid() const noexcept {
        return has_bid() ? base_tick_ + best_bid_idx_ : no_price;
    }
    [[nodiscard]] price_t best_ask() const noexcept {
        return has_ask() ? base_tick_ + best_ask_idx_ : no_price;
    }

    [[nodiscard]] qty_t best_bid_qty() const noexcept {
        return has_bid() ? bid_levels_[static_cast<std::size_t>(best_bid_idx_)].total_qty : 0;
    }
    [[nodiscard]] qty_t best_ask_qty() const noexcept {
        return has_ask() ? ask_levels_[static_cast<std::size_t>(best_ask_idx_)].total_qty : 0;
    }

    // spread in ticks; only meaningful when both sides are populated.
    [[nodiscard]] price_t spread() const noexcept {
        return (has_bid() && has_ask()) ? best_ask() - best_bid() : no_price;
    }

    // arithmetic mid in ticks. returns nan when a side is missing.
    [[nodiscard]] double mid() const noexcept {
        if (!has_bid() || !has_ask()) [[unlikely]] {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return 0.5 * static_cast<double>(best_bid() + best_ask());
    }

    // size-weighted micro-price in ticks. weights each side's price by the
    // opposite side's resting size, the standard fair-value estimator at the top
    // of book. this is precisely the quantity the fpga feature engine in later
    // phases will compute in hardware; here it is the reference implementation.
    [[nodiscard]] double micro_price() const noexcept {
        if (!has_bid() || !has_ask()) [[unlikely]] {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const double bq = static_cast<double>(best_bid_qty());
        const double aq = static_cast<double>(best_ask_qty());
        const double denom = bq + aq;
        if (denom == 0.0) [[unlikely]] {
            return mid();
        }
        return (static_cast<double>(best_bid()) * aq +
                static_cast<double>(best_ask()) * bq) /
               denom;
    }

    // order-flow imbalance over the top `depth` levels of each side, in
    // [-1, +1]: +1 is all bid, -1 is all ask. returns 0 on an empty book.
    [[nodiscard]] double imbalance(std::size_t depth = 1) const noexcept {
        const std::uint64_t bid = side_depth_qty(side::bid, depth);
        const std::uint64_t ask = side_depth_qty(side::ask, depth);
        const double denom      = static_cast<double>(bid + ask);
        if (denom == 0.0) [[unlikely]] {
            return 0.0;
        }
        return (static_cast<double>(bid) - static_cast<double>(ask)) / denom;
    }

    [[nodiscard]] std::size_t live_orders() const noexcept { return pool_.in_use(); }
    [[nodiscard]] price_t base_tick() const noexcept { return base_tick_; }
    [[nodiscard]] static constexpr std::size_t num_ticks() noexcept { return NumTicks; }
    [[nodiscard]] static constexpr std::size_t max_orders() noexcept { return MaxOrders; }

    // ---- read-only queries used by the fill simulator ---------------------

    // look up a resting order by id. on success writes its side / price (ticks) /
    // remaining qty & returns true. used by the engine to resolve the price &
    // side of an execute / cancel / delete event *before* it mutates the book, so
    // the fill model can attribute consumed queue volume to the right level.
    [[nodiscard]] hft_hot bool peek_order(order_id_t id, side& s, price_t& price,
                                          qty_t& qty) const noexcept {
        const slot_t slot = index_.find(id);
        if (slot == invalid_slot) [[unlikely]] {
            return false;
        }
        const order& o = pool_[slot];
        s     = o.s;
        price = base_tick_ + static_cast<price_t>(o.price_idx);
        qty   = o.qty;
        return true;
    }

    // total resting quantity at one price on one side (0 if empty / out of band).
    // this is the volume with time priority ahead of a freshly-posted order, i.e.
    // its initial queue position.
    [[nodiscard]] hft_hot qty_t level_qty(side s, price_t price) const noexcept {
        const std::size_t idx = tick_index(price);
        if (idx >= NumTicks) [[unlikely]] {
            return 0;
        }
        return level_array(s)[idx].total_qty;
    }

    // walk populated levels of one side from the inside out, calling
    // fn(price_t price, qty_t qty) for each; iteration stops when fn returns false
    // or max_levels populated levels have been visited. read-only -- used by the
    // taker sweep to price marketable orders against displayed liquidity without
    // mutating the (feed-driven) book.
    template <typename Fn>
    hft_hot void walk(side s, std::size_t max_levels, Fn&& fn) const {
        std::size_t seen = 0;
        if (s == side::bid) {
            for (std::int64_t i = best_bid_idx_; i >= 0 && seen < max_levels; --i) {
                const price_level& lvl = bid_levels_[static_cast<std::size_t>(i)];
                if (lvl.order_count != 0) {
                    if (!fn(base_tick_ + i, lvl.total_qty)) return;
                    ++seen;
                }
            }
        } else {
            for (std::int64_t i = best_ask_idx_;
                 i >= 0 && static_cast<std::size_t>(i) < NumTicks && seen < max_levels; ++i) {
                const price_level& lvl = ask_levels_[static_cast<std::size_t>(i)];
                if (lvl.order_count != 0) {
                    if (!fn(base_tick_ + i, lvl.total_qty)) return;
                    ++seen;
                }
            }
        }
    }

private:
    // map a price to its level index; values >= NumTicks are out of band. the
    // unsigned cast folds the negative (below base) case into the same check.
    [[nodiscard]] hft_always_inline std::size_t tick_index(price_t price) const noexcept {
        return static_cast<std::size_t>(price - base_tick_);
    }

    [[nodiscard]] hft_always_inline price_level* level_array(side s) noexcept {
        return s == side::bid ? bid_levels_.get() : ask_levels_.get();
    }
    [[nodiscard]] hft_always_inline const price_level* level_array(side s) const noexcept {
        return s == side::bid ? bid_levels_.get() : ask_levels_.get();
    }

    // unlink one order from its level's fifo queue & free its slot. shared by
    // cancel, execute (full fill) & replace.
    hft_always_inline void remove_slot(slot_t slot) noexcept {
        order& o         = pool_[slot];
        const side s     = o.s;
        const std::size_t idx = o.price_idx;
        price_level& lvl = level_array(s)[idx];

        if (o.prev != invalid_slot) {
            pool_[o.prev].next = o.next;
        } else {
            lvl.head = o.next;  // removed the front of the queue
        }
        if (o.next != invalid_slot) {
            pool_[o.next].prev = o.prev;
        } else {
            lvl.tail = o.prev;  // removed the back of the queue
        }

        lvl.total_qty -= o.qty;
        --lvl.order_count;
        pool_.release(slot);

        if (lvl.order_count == 0) [[unlikely]] {
            demote_best(s, idx);
        }
    }

    // a new order may improve the best price on its side; update the cached index.
    hft_always_inline void promote_best(side s, std::size_t idx) noexcept {
        const std::int64_t i = static_cast<std::int64_t>(idx);
        if (s == side::bid) {
            if (i > best_bid_idx_) best_bid_idx_ = i;  // higher bid is better
        } else {
            if (best_ask_idx_ < 0 || i < best_ask_idx_) best_ask_idx_ = i;  // lower ask is better
        }
    }

    // the best level on side s just emptied at index idx; re-scan the contiguous
    // level array for the next populated level. only runs when a top level fully
    // clears, & the walk is over packed memory so it prefetches perfectly.
    hft_noinline hft_cold void demote_best(side s, std::size_t idx) noexcept {
        if (s == side::bid) {
            if (static_cast<std::int64_t>(idx) != best_bid_idx_) return;
            std::int64_t i = best_bid_idx_ - 1;
            while (i >= 0 && bid_levels_[static_cast<std::size_t>(i)].order_count == 0) --i;
            best_bid_idx_ = i;  // -1 when the side is now empty
        } else {
            if (static_cast<std::int64_t>(idx) != best_ask_idx_) return;
            std::size_t i = idx + 1;
            while (i < NumTicks && ask_levels_[i].order_count == 0) ++i;
            best_ask_idx_ = (i < NumTicks) ? static_cast<std::int64_t>(i) : -1;
        }
    }

    // sum resting quantity over the best `depth` populated levels of one side.
    [[nodiscard]] std::uint64_t side_depth_qty(side s, std::size_t depth) const noexcept {
        std::uint64_t total = 0;
        std::size_t   seen  = 0;
        if (s == side::bid) {
            for (std::int64_t i = best_bid_idx_; i >= 0 && seen < depth; --i) {
                const price_level& lvl = bid_levels_[static_cast<std::size_t>(i)];
                if (lvl.order_count != 0) {
                    total += lvl.total_qty;
                    ++seen;
                }
            }
        } else {
            for (std::int64_t i = best_ask_idx_;
                 i >= 0 && static_cast<std::size_t>(i) < NumTicks && seen < depth; ++i) {
                const price_level& lvl = ask_levels_[static_cast<std::size_t>(i)];
                if (lvl.order_count != 0) {
                    total += lvl.total_qty;
                    ++seen;
                }
            }
        }
        return total;
    }

    // hot, frequently-read state kept together & isolated on its own line so
    // it never false-shares with the larger backing arrays below.
    hft_cache_aligned price_t      base_tick_;
    std::int64_t                   best_bid_idx_ = -1;  // -1 == no bids
    std::int64_t                   best_ask_idx_ = -1;  // -1 == no asks

    std::unique_ptr<price_level[]> bid_levels_;
    std::unique_ptr<price_level[]> ask_levels_;
    object_pool<order, MaxOrders>  pool_;
    order_index_map<IndexCapacity> index_;
};

}  // namespace hft
