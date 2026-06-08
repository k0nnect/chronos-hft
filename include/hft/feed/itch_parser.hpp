// zero-copy decoder for the itch-like wire protocol.
//
// `frame_cursor` walks a contiguous buffer of length-prefixed frames, handing
// back a pointer + length for each message body without copying it. `decode`
// turns one message body into a normalized market_event by reading its fields
// in order with load_be. nothing here allocates, throws, or copies the payload;
// the only write is into the caller-supplied event. truncated input (a partial
// final frame, common when reading from a socket) is reported, not crashed on.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hft/core/byte_order.hpp"
#include "hft/core/compiler.hpp"
#include "hft/feed/itch_protocol.hpp"
#include "hft/feed/market_event.hpp"

namespace hft::itch {

// sequential big-endian field reader over one message body. callers gate reads
// behind a single up-front size check (via the *_msg sizeof constants), so the
// per-field reads themselves stay branch-free.
class field_reader {
public:
    explicit field_reader(const std::uint8_t* p) noexcept : p_(p) {}

    template <typename T>
    [[nodiscard]] hft_always_inline T be() noexcept {
        const T v = load_be<T>(p_);
        p_ += sizeof(T);
        return v;
    }

    [[nodiscard]] hft_always_inline char ch() noexcept {
        return static_cast<char>(*p_++);
    }

    hft_always_inline void skip(std::size_t n) noexcept { p_ += n; }

private:
    const std::uint8_t* p_;
};

// decode one message body of `len` bytes into `out`.
// returns false (leaving out unspecified) on an unknown type or a body too short
// for its declared type -- i.e. a malformed or truncated message.
[[nodiscard]] hft_hot inline bool decode(const std::uint8_t* body, std::size_t len,
                                         market_event& out) noexcept {
    if (len < 1) [[unlikely]] {
        return false;
    }
    const char type = static_cast<char>(body[0]);

    switch (type) {
        case msg_type::add: {
            if (len < sizeof(add_order_msg)) [[unlikely]] return false;
            field_reader r(body + 1);  // skip the type byte
            out.type         = event_type::add;
            out.stock_locate = r.be<std::uint16_t>();
            out.timestamp    = r.be<std::uint64_t>();
            out.order_id     = r.be<std::uint64_t>();
            out.s            = (r.ch() == 'B') ? side::bid : side::ask;
            out.qty          = r.be<std::uint32_t>();
            out.price        = static_cast<price_t>(r.be<std::uint32_t>());
            out.new_order_id = 0;
            out.match_number = 0;
            return true;
        }
        case msg_type::execute: {
            if (len < sizeof(order_executed_msg)) [[unlikely]] return false;
            field_reader r(body + 1);
            out.type         = event_type::execute;
            out.stock_locate = r.be<std::uint16_t>();
            out.timestamp    = r.be<std::uint64_t>();
            out.order_id     = r.be<std::uint64_t>();
            out.qty          = r.be<std::uint32_t>();
            out.match_number = r.be<std::uint64_t>();
            out.new_order_id = 0;
            out.price        = 0;
            return true;
        }
        case msg_type::reduce: {
            if (len < sizeof(order_cancel_msg)) [[unlikely]] return false;
            field_reader r(body + 1);
            out.type         = event_type::reduce;
            out.stock_locate = r.be<std::uint16_t>();
            out.timestamp    = r.be<std::uint64_t>();
            out.order_id     = r.be<std::uint64_t>();
            out.qty          = r.be<std::uint32_t>();
            out.new_order_id = 0;
            out.price        = 0;
            out.match_number = 0;
            return true;
        }
        case msg_type::delete_order: {
            if (len < sizeof(order_delete_msg)) [[unlikely]] return false;
            field_reader r(body + 1);
            out.type         = event_type::delete_order;
            out.stock_locate = r.be<std::uint16_t>();
            out.timestamp    = r.be<std::uint64_t>();
            out.order_id     = r.be<std::uint64_t>();
            out.qty          = 0;
            out.new_order_id = 0;
            out.price        = 0;
            out.match_number = 0;
            return true;
        }
        case msg_type::replace: {
            if (len < sizeof(order_replace_msg)) [[unlikely]] return false;
            field_reader r(body + 1);
            out.type         = event_type::replace;
            out.stock_locate = r.be<std::uint16_t>();
            out.timestamp    = r.be<std::uint64_t>();
            out.order_id     = r.be<std::uint64_t>();  // original ref
            out.new_order_id = r.be<std::uint64_t>();  // replacement ref
            out.qty          = r.be<std::uint32_t>();
            out.price        = static_cast<price_t>(r.be<std::uint32_t>());
            out.match_number = 0;
            return true;
        }
        case msg_type::trade: {
            if (len < sizeof(trade_msg)) [[unlikely]] return false;
            field_reader r(body + 1);
            out.type         = event_type::trade;
            out.stock_locate = r.be<std::uint16_t>();
            out.timestamp    = r.be<std::uint64_t>();
            out.order_id     = r.be<std::uint64_t>();
            out.s            = (r.ch() == 'B') ? side::bid : side::ask;
            out.qty          = r.be<std::uint32_t>();
            out.price        = static_cast<price_t>(r.be<std::uint32_t>());
            out.match_number = r.be<std::uint64_t>();
            out.new_order_id = 0;
            return true;
        }
        default:
            return false;  // unknown message type
    }
}

// walks a buffer of [be16 length][body] frames. designed for streaming: a
// partial trailing frame simply ends iteration, & `consumed()` reports how
// many bytes were fully processed so the caller can carry the remainder forward.
class frame_cursor {
public:
    frame_cursor(const std::uint8_t* data, std::size_t len) noexcept
        : base_(data), cur_(data), end_(data + len) {}

    // returns the next message body & writes its length to out_len, or nullptr
    // when the buffer is exhausted or only a partial frame remains.
    [[nodiscard]] hft_hot const std::uint8_t* next(std::uint16_t& out_len) noexcept {
        if (cur_ + frame_header_size > end_) [[unlikely]] {
            return nullptr;  // not even a length prefix left
        }
        const std::uint16_t len = load_be<std::uint16_t>(cur_);
        const std::uint8_t* body = cur_ + frame_header_size;
        if (body + len > end_) [[unlikely]] {
            return nullptr;  // body straddles the end of the buffer (truncated)
        }
        cur_   = body + len;
        out_len = len;
        return body;
    }

    [[nodiscard]] bool exhausted() const noexcept { return cur_ >= end_; }
    [[nodiscard]] std::size_t consumed() const noexcept {
        return static_cast<std::size_t>(cur_ - base_);
    }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return static_cast<std::size_t>(end_ - cur_);
    }

private:
    const std::uint8_t* base_;
    const std::uint8_t* cur_;
    const std::uint8_t* end_;
};

}  // namespace hft::itch
