// the simulated itch-like binary wire protocol.
//
// framing follows the soupbintcp/moldudp convention used to carry real itch: a
// 2-byte big-endian length prefix followed by that many bytes of message body.
// the first body byte is the message type; the remaining fields are fixed-width
// & big-endian. the packed structs below are the authoritative description of
// each message's on-wire layout; they exist so the static_asserts pin the byte
// sizes the encoder/decoder rely on. the decoder never overlays them on the
// buffer (that would be an aliasing hazard) -- it reads fields sequentially with
// load_be, which is both alignment-safe & strict-aliasing-safe.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hft::itch {

// 2-byte big-endian frame length prefix.
inline constexpr std::size_t frame_header_size = 2;

namespace msg_type {
inline constexpr char add          = 'A';  // add order
inline constexpr char execute      = 'E';  // order executed
inline constexpr char reduce       = 'X';  // order cancel (partial, shares)
inline constexpr char delete_order = 'D';  // order delete (full)
inline constexpr char replace      = 'U';  // order replace
inline constexpr char trade        = 'P';  // trade (non-cross / hidden)
}  // namespace msg_type

#pragma pack(push, 1)

// 'A' -- add order. side is 'B' (buy/bid) or 'S' (sell/ask).
struct add_order_msg {
    char          message_type;
    std::uint16_t stock_locate;
    std::uint64_t timestamp;
    std::uint64_t order_ref;
    char          side;
    std::uint32_t shares;
    std::uint32_t price;
};
static_assert(sizeof(add_order_msg) == 28, "add_order wire size");

// 'E' -- order executed against a resting order.
struct order_executed_msg {
    char          message_type;
    std::uint16_t stock_locate;
    std::uint64_t timestamp;
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
};
static_assert(sizeof(order_executed_msg) == 31, "order_executed wire size");

// 'X' -- partial cancel: `cancelled_shares` removed from a resting order.
struct order_cancel_msg {
    char          message_type;
    std::uint16_t stock_locate;
    std::uint64_t timestamp;
    std::uint64_t order_ref;
    std::uint32_t cancelled_shares;
};
static_assert(sizeof(order_cancel_msg) == 23, "order_cancel wire size");

// 'D' -- full delete of a resting order.
struct order_delete_msg {
    char          message_type;
    std::uint16_t stock_locate;
    std::uint64_t timestamp;
    std::uint64_t order_ref;
};
static_assert(sizeof(order_delete_msg) == 19, "order_delete wire size");

// 'U' -- replace: cancels original_order_ref, adds new_order_ref at new px/qty.
struct order_replace_msg {
    char          message_type;
    std::uint16_t stock_locate;
    std::uint64_t timestamp;
    std::uint64_t original_order_ref;
    std::uint64_t new_order_ref;
    std::uint32_t shares;
    std::uint32_t price;
};
static_assert(sizeof(order_replace_msg) == 35, "order_replace wire size");

// 'P' -- trade print (does not alter the displayed book).
struct trade_msg {
    char          message_type;
    std::uint16_t stock_locate;
    std::uint64_t timestamp;
    std::uint64_t order_ref;
    char          side;
    std::uint32_t shares;
    std::uint32_t price;
    std::uint64_t match_number;
};
static_assert(sizeof(trade_msg) == 36, "trade wire size");

#pragma pack(pop)

// the largest message body; useful for sizing scratch buffers.
inline constexpr std::size_t max_message_size = sizeof(trade_msg);

}  // namespace hft::itch
