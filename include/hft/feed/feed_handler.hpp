// feed handler: drives raw bytes through the frame cursor and decoder, emitting
// normalized market_events to a sink.
//
// the sink is a caller-supplied callable so the handler is decoupled from what
// happens downstream -- in tests it appends to a vector, in the live path it is
// `ring_sink` which publishes into the spsc ring for the engine thread. parsing
// is allocation-free; the only state kept is a small set of counters for
// observability (parsed / malformed / bytes consumed).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hft/core/compiler.hpp"
#include "hft/feed/itch_parser.hpp"
#include "hft/feed/market_event.hpp"
#include "hft/feed/spsc_ring.hpp"

namespace hft {

struct feed_stats {
    std::uint64_t messages   = 0;  // successfully decoded
    std::uint64_t malformed  = 0;  // bad type or short body
    std::uint64_t bytes      = 0;  // fully-consumed bytes
};

class feed_handler {
public:
    // decode every complete frame in [data, data+len) and pass each decoded
    // event to sink(const market_event&). returns the number of bytes fully
    // consumed; any trailing partial frame is left for the caller to re-present.
    template <typename Sink>
    hft_hot std::size_t process(const std::uint8_t* data, std::size_t len, Sink&& sink) {
        itch::frame_cursor cursor(data, len);
        market_event       ev;
        std::uint16_t      body_len = 0;

        for (const std::uint8_t* body = cursor.next(body_len); body != nullptr;
             body = cursor.next(body_len)) {
            if (itch::decode(body, body_len, ev)) [[likely]] {
                ++stats_.messages;
                sink(ev);
            } else {
                ++stats_.malformed;
            }
        }
        stats_.bytes += cursor.consumed();
        return cursor.consumed();
    }

    [[nodiscard]] const feed_stats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = feed_stats{}; }

private:
    feed_stats stats_;
};

// sink that publishes decoded events into an spsc ring, spinning on a full ring
// so no event is ever dropped. `spins` accumulates how many times the producer
// had to wait, which is a direct measure of downstream backpressure.
template <typename Ring>
class ring_sink {
public:
    explicit ring_sink(Ring& ring) noexcept : ring_(&ring) {}

    hft_always_inline void operator()(const market_event& ev) noexcept {
        while (!ring_->try_push(ev)) [[unlikely]] {
            ++spins_;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
    }

    [[nodiscard]] std::uint64_t spins() const noexcept { return spins_; }

private:
    Ring*         ring_;
    std::uint64_t spins_ = 0;
};

}  // namespace hft
