// fixed-capacity object pool with an O(1) intrusive free list.
//
// all storage is allocated once, up front, from the heap at construction time
// and never touched by the allocator again. on the hot path acquire()/release()
// are a single array write plus an index bump, so there is no malloc, no locks
// and no pointer chasing through freed nodes. handles are dense 32-bit indices
// rather than pointers, which keeps cross-references half the size and pointer
// stable across the lifetime of the pool.
#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include "hft/core/cache.hpp"
#include "hft/core/compiler.hpp"
#include "hft/core/types.hpp"

namespace hft {

template <typename T, std::size_t Capacity>
class object_pool {
    static_assert(Capacity > 0, "pool capacity must be non-zero");
    static_assert(Capacity < invalid_slot, "capacity collides with invalid_slot sentinel");

public:
    object_pool()
        : storage_(std::make_unique<T[]>(Capacity)),
          free_stack_(std::make_unique<slot_t[]>(Capacity)) {
        reset();
    }

    object_pool(const object_pool&)            = delete;
    object_pool& operator=(const object_pool&) = delete;
    object_pool(object_pool&&)                 = default;
    object_pool& operator=(object_pool&&)      = default;

    // return every slot to the free list. ordered so the first acquire() hands
    // out slot 0, then 1, ... which keeps early allocations contiguous and warm.
    void reset() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            free_stack_[i] = static_cast<slot_t>(Capacity - 1 - i);
        }
        free_top_ = Capacity;
    }

    // pop a free slot. returns invalid_slot when the pool is exhausted so the
    // caller can reject the order rather than crash mid-burst.
    [[nodiscard]] hft_always_inline slot_t acquire() noexcept {
        if (free_top_ == 0) [[unlikely]] {
            return invalid_slot;
        }
        return free_stack_[--free_top_];
    }

    hft_always_inline void release(slot_t s) noexcept {
        free_stack_[free_top_++] = s;
    }

    [[nodiscard]] hft_always_inline T& operator[](slot_t s) noexcept {
        return storage_[s];
    }
    [[nodiscard]] hft_always_inline const T& operator[](slot_t s) const noexcept {
        return storage_[s];
    }

    [[nodiscard]] std::size_t in_use() const noexcept { return Capacity - free_top_; }
    [[nodiscard]] std::size_t available() const noexcept { return free_top_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    std::unique_ptr<T[]>      storage_;
    std::unique_ptr<slot_t[]> free_stack_;
    std::size_t               free_top_ = 0;
};

}  // namespace hft
