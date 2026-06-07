// fundamental fixed-width domain types shared across the whole system.
// prices are integer ticks (never floating point on the hot path), quantities
// and identifiers are unsigned, and slot/level handles are 32-bit so two of
// them pack into a single 64-bit word.
#pragma once

#include <cstdint>

namespace hft {

using price_t     = std::int64_t;   // price expressed in integer ticks
using qty_t       = std::uint64_t;  // share / lot quantity
using order_id_t  = std::uint64_t;  // exchange order reference
using slot_t      = std::uint32_t;  // index into the order pool
using level_idx_t = std::uint32_t;  // index into a side's price-level array
using ts_t        = std::uint64_t;  // nanoseconds since an arbitrary epoch
using seq_t       = std::uint64_t;  // monotonically increasing event sequence

// sentinels. chosen as the max value of the underlying type so a freshly
// zeroed structure never accidentally aliases a valid handle.
inline constexpr slot_t      invalid_slot  = 0xFFFFFFFFu;
inline constexpr level_idx_t invalid_level = 0xFFFFFFFFu;
inline constexpr price_t     no_price      = INT64_MIN;

enum class side : std::uint8_t { bid = 0, ask = 1 };

[[nodiscard]] constexpr side opposite(side s) noexcept {
    return s == side::bid ? side::ask : side::bid;
}

[[nodiscard]] constexpr const char* to_string(side s) noexcept {
    return s == side::bid ? "bid" : "ask";
}

}  // namespace hft
