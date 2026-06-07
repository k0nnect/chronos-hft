// lock-free single-producer / single-consumer ring buffer.
//
// this is the wire between the feed-handler thread and the engine thread. it is
// wait-free for both sides: a push or pop is a relaxed load, one bounds compare,
// a slot copy and a release store. the head/tail indices live on separate cache
// lines, and each side keeps a private *cached* copy of the other side's index
// so the common case never reads the contended atomic at all -- this is the key
// trick that keeps the two cores from ping-ponging the cache line that owns the
// shared counter (the dominant cost in a naive spsc queue).
//
// indices are free-running 64-bit counters; capacity is a power of two so the
// slot is just `index & mask`. a u64 counter never realistically wraps, which
// sidesteps the empty/full ambiguity of wrap-around index schemes.
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

#include "hft/core/cache.hpp"
#include "hft/core/compiler.hpp"

namespace hft {

template <typename T, std::size_t Capacity>
class spsc_ring {
    static_assert(Capacity >= 2, "capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "spsc_ring payload must be trivially copyable");

    static constexpr std::size_t mask = Capacity - 1;

public:
    spsc_ring() : buffer_(std::make_unique<T[]>(Capacity)) {}

    spsc_ring(const spsc_ring&)            = delete;
    spsc_ring& operator=(const spsc_ring&) = delete;

    // producer side. returns false if the ring is full (no overwrite, ever).
    [[nodiscard]] hft_hot bool try_push(const T& value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ >= Capacity) [[unlikely]] {
            // our cached view says full; refresh it from the consumer once.
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ >= Capacity) [[unlikely]] {
                return false;
            }
        }
        buffer_[tail & mask] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // consumer side. returns false if the ring is empty.
    [[nodiscard]] hft_hot bool try_pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) [[unlikely]] {
            // our cached view says empty; refresh it from the producer once.
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) [[unlikely]] {
                return false;
            }
        }
        out = buffer_[head & mask];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // best-effort occupancy snapshot. exact only when quiescent; for metrics.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // pointer is read-only after construction; keep it off the hot atomic lines.
    std::unique_ptr<T[]> buffer_;

    // consumer-owned line: the head counter the consumer advances, plus its
    // private cache of the producer's tail. isolated so producer writes to tail_
    // never invalidate this line for the consumer.
    hft_cache_aligned std::atomic<std::size_t> head_{0};
    std::size_t                                tail_cache_ = 0;

    // producer-owned line: the tail counter the producer advances, plus its
    // private cache of the consumer's head.
    hft_cache_aligned std::atomic<std::size_t> tail_{0};
    std::size_t                                head_cache_ = 0;

    // trailing pad so a neighbouring object cannot land on the producer line.
    char pad_[cacheline_size - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)];
};

}  // namespace hft
