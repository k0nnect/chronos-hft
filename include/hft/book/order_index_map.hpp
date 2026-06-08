// open-addressing hash map from exchange order id -> pool slot.
//
// linear probing over two parallel power-of-two arrays. keys & values are
// stored separately so a probe walk touches only the dense key array until it
// finds the match, which is far friendlier to the cache than a node-per-entry
// std::unordered_map. all memory is reserved up front; nothing allocates on the
// hot path. two key values are reserved as sentinels (empty / tombstone), which
// is safe because real exchange order ids never reach the top of the u64 range.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "hft/core/compiler.hpp"
#include "hft/core/types.hpp"

namespace hft {

template <std::size_t Capacity>
class order_index_map {
    static_assert(Capacity >= 2, "capacity too small");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

    static constexpr order_id_t empty_key     = 0xFFFFFFFFFFFFFFFFull;
    static constexpr order_id_t tombstone_key = 0xFFFFFFFFFFFFFFFEull;
    static constexpr std::size_t mask         = Capacity - 1;

public:
    order_index_map()
        : keys_(std::make_unique<order_id_t[]>(Capacity)),
          vals_(std::make_unique<slot_t[]>(Capacity)) {
        clear();
    }

    void clear() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            keys_[i] = empty_key;
        }
        size_ = 0;
    }

    // returns false if the table is full or the id already exists.
    [[nodiscard]] hft_always_inline bool insert(order_id_t id, slot_t slot) noexcept {
        if (size_ >= Capacity) [[unlikely]] {
            return false;
        }
        std::size_t i        = index_of(id);
        std::size_t insert_at = Capacity;  // first tombstone seen, for reuse
        while (true) {
            const order_id_t k = keys_[i];
            if (k == empty_key) {
                const std::size_t at = (insert_at != Capacity) ? insert_at : i;
                keys_[at] = id;
                vals_[at] = slot;
                ++size_;
                return true;
            }
            if (k == id) [[unlikely]] {
                return false;  // duplicate id
            }
            if (k == tombstone_key && insert_at == Capacity) {
                insert_at = i;
            }
            i = (i + 1) & mask;
        }
    }

    // returns the slot for id, or invalid_slot if absent.
    [[nodiscard]] hft_always_inline slot_t find(order_id_t id) const noexcept {
        std::size_t i = index_of(id);
        while (true) {
            const order_id_t k = keys_[i];
            if (k == id) {
                return vals_[i];
            }
            if (k == empty_key) [[unlikely]] {
                return invalid_slot;
            }
            i = (i + 1) & mask;
        }
    }

    // returns the slot that was removed, or invalid_slot if the id was absent.
    [[nodiscard]] hft_always_inline slot_t erase(order_id_t id) noexcept {
        std::size_t i = index_of(id);
        while (true) {
            const order_id_t k = keys_[i];
            if (k == id) {
                const slot_t slot = vals_[i];
                keys_[i]          = tombstone_key;
                --size_;
                return slot;
            }
            if (k == empty_key) [[unlikely]] {
                return invalid_slot;
            }
            i = (i + 1) & mask;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // splitmix64 finaliser: cheap, branch-free, & scrambles the low bits that
    // linear probing keys off, which sequential exchange ids otherwise lack.
    [[nodiscard]] hft_always_inline static std::size_t index_of(order_id_t x) noexcept {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        x = x ^ (x >> 31);
        return static_cast<std::size_t>(x) & mask;
    }

    std::unique_ptr<order_id_t[]> keys_;
    std::unique_ptr<slot_t[]>     vals_;
    std::size_t                   size_ = 0;
};

}  // namespace hft
