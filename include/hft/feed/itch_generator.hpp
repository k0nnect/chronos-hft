// synthetic itch-like feed generator + encoders.
//
// the encoders write length-prefixed big-endian frames that the parser decodes,
// so encode/decode are exact inverses. `generate_feed` runs a small market
// simulator that keeps a coherent, *uncrossed* limit order book: liquidity joins
// at and around the touch, aggressors trade through it (emitted as executes),
// and resting orders are cancelled. the touch random-walks within a price band.
// because orders join behind one another in fifo order, a passive order resting
// at the touch is genuinely filled once the queue ahead of it trades out -- which
// is what lets a maker strategy earn realistic fills downstream.
//
// the generator reports the exact resting-order count the book must hold after
// replay, and every emitted message references a live order with a valid
// quantity, so applying the stream to an order_book never rejects an operation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <random>
#include <utility>
#include <vector>

#include "hft/core/byte_order.hpp"
#include "hft/core/types.hpp"
#include "hft/feed/itch_protocol.hpp"

namespace hft::itch {

// appends one length-prefixed frame, back-patching the 2-byte length on scope
// exit so message bodies can be written field-by-field without pre-counting.
class frame_writer {
public:
    explicit frame_writer(std::vector<std::uint8_t>& buf) : buf_(buf), len_pos_(buf.size()) {
        buf_.push_back(0);  // length placeholder (2 bytes)
        buf_.push_back(0);
    }

    ~frame_writer() {
        const std::size_t body = buf_.size() - len_pos_ - frame_header_size;
        store_be<std::uint16_t>(&buf_[len_pos_], static_cast<std::uint16_t>(body));
    }

    frame_writer(const frame_writer&)            = delete;
    frame_writer& operator=(const frame_writer&) = delete;

    void ch(char c) { buf_.push_back(static_cast<std::uint8_t>(c)); }

    template <typename T>
    void be(T v) {
        std::uint8_t tmp[sizeof(T)];
        store_be<T>(tmp, v);
        buf_.insert(buf_.end(), tmp, tmp + sizeof(T));
    }

private:
    std::vector<std::uint8_t>& buf_;
    std::size_t                len_pos_;
};

inline void encode_add(std::vector<std::uint8_t>& buf, std::uint16_t stock, ts_t ts,
                       order_id_t ref, side s, qty_t shares, price_t price) {
    frame_writer w(buf);
    w.ch(msg_type::add);
    w.be<std::uint16_t>(stock);
    w.be<std::uint64_t>(ts);
    w.be<std::uint64_t>(ref);
    w.ch(s == side::bid ? 'B' : 'S');
    w.be<std::uint32_t>(static_cast<std::uint32_t>(shares));
    w.be<std::uint32_t>(static_cast<std::uint32_t>(price));
}

inline void encode_execute(std::vector<std::uint8_t>& buf, std::uint16_t stock, ts_t ts,
                           order_id_t ref, qty_t executed, std::uint64_t match) {
    frame_writer w(buf);
    w.ch(msg_type::execute);
    w.be<std::uint16_t>(stock);
    w.be<std::uint64_t>(ts);
    w.be<std::uint64_t>(ref);
    w.be<std::uint32_t>(static_cast<std::uint32_t>(executed));
    w.be<std::uint64_t>(match);
}

inline void encode_cancel(std::vector<std::uint8_t>& buf, std::uint16_t stock, ts_t ts,
                          order_id_t ref, qty_t cancelled) {
    frame_writer w(buf);
    w.ch(msg_type::reduce);
    w.be<std::uint16_t>(stock);
    w.be<std::uint64_t>(ts);
    w.be<std::uint64_t>(ref);
    w.be<std::uint32_t>(static_cast<std::uint32_t>(cancelled));
}

inline void encode_delete(std::vector<std::uint8_t>& buf, std::uint16_t stock, ts_t ts,
                          order_id_t ref) {
    frame_writer w(buf);
    w.ch(msg_type::delete_order);
    w.be<std::uint16_t>(stock);
    w.be<std::uint64_t>(ts);
    w.be<std::uint64_t>(ref);
}

inline void encode_replace(std::vector<std::uint8_t>& buf, std::uint16_t stock, ts_t ts,
                           order_id_t orig, order_id_t fresh, qty_t shares, price_t price) {
    frame_writer w(buf);
    w.ch(msg_type::replace);
    w.be<std::uint16_t>(stock);
    w.be<std::uint64_t>(ts);
    w.be<std::uint64_t>(orig);
    w.be<std::uint64_t>(fresh);
    w.be<std::uint32_t>(static_cast<std::uint32_t>(shares));
    w.be<std::uint32_t>(static_cast<std::uint32_t>(price));
}

inline void encode_trade(std::vector<std::uint8_t>& buf, std::uint16_t stock, ts_t ts,
                         order_id_t ref, side s, qty_t shares, price_t price,
                         std::uint64_t match) {
    frame_writer w(buf);
    w.ch(msg_type::trade);
    w.be<std::uint16_t>(stock);
    w.be<std::uint64_t>(ts);
    w.be<std::uint64_t>(ref);
    w.ch(s == side::bid ? 'B' : 'S');
    w.be<std::uint32_t>(static_cast<std::uint32_t>(shares));
    w.be<std::uint32_t>(static_cast<std::uint32_t>(price));
    w.be<std::uint64_t>(match);
}

// the byte stream plus the ground truth a consumer can check itself against.
struct synthetic_feed {
    std::vector<std::uint8_t> bytes;              // the wire stream
    std::uint64_t             messages      = 0;  // total frames emitted
    std::uint64_t             adds          = 0;
    std::uint64_t             executes      = 0;
    std::uint64_t             cancels       = 0;
    std::uint64_t             deletes       = 0;
    std::uint64_t             replaces      = 0;
    std::uint64_t             trades        = 0;
    std::size_t               expected_live = 0;  // resting orders after replay
    price_t                   base_tick     = 0;  // lowest price representable
    std::size_t               tick_band     = 0;  // price span the book must cover
};

// generate a deterministic, internally-consistent, *uncrossed* market: liquidity
// at the touch, aggressors trading through it, and cancels. seeded by `seed`.
inline synthetic_feed generate_feed(std::size_t message_count, std::uint64_t seed = 0xA5A5A5A5) {
    synthetic_feed feed;
    feed.base_tick = 1'000'000;
    feed.tick_band = 4096;
    feed.bytes.reserve(message_count * 20);

    // keep the touch comfortably inside the band so depth never leaves it.
    const price_t     lo    = feed.base_tick + 1024;
    const price_t     hi    = feed.base_tick + 3072;
    const std::uint16_t stock = 1;

    std::mt19937_64                              rng(seed);
    std::uniform_int_distribution<std::uint64_t> qty_dist(1, 500);
    std::uniform_int_distribution<int>           depth_dist(0, 3);
    std::uniform_int_distribution<int>           action(0, 99);

    // a price level is a fifo queue of (order id, remaining qty). the maps are
    // ordered, so the best bid is bids.rbegin() and the best ask is asks.begin().
    using level = std::deque<std::pair<order_id_t, qty_t>>;
    std::map<price_t, level> bids;
    std::map<price_t, level> asks;

    order_id_t    next_id    = 1;
    ts_t          ts         = 0;
    std::uint64_t next_match = 1;
    std::size_t   live_count = 0;

    auto best_bid = [&]() -> price_t { return bids.empty() ? no_price : bids.rbegin()->first; };
    auto best_ask = [&]() -> price_t { return asks.empty() ? no_price : asks.begin()->first; };

    auto emit_add = [&](side s, price_t px) {
        const qty_t q = qty_dist(rng);
        encode_add(feed.bytes, stock, ts++, next_id, s, q, px);
        (s == side::bid ? bids : asks)[px].push_back({next_id, q});
        ++next_id;
        ++live_count;
        ++feed.adds;
    };

    // seed a one-tick-wide market at the centre of the band.
    auto seed_bid = [&]() { emit_add(side::bid, (lo + hi) / 2 - 1); };
    auto seed_ask = [&]() { emit_add(side::ask, (lo + hi) / 2 + 1); };

    // consume `from` the front of the inside level on one side via an execute.
    auto aggress = [&](side resting_side) {
        auto& book = (resting_side == side::bid) ? bids : asks;
        auto  it   = (resting_side == side::bid) ? std::prev(book.end()) : book.begin();
        auto& dq   = it->second;
        auto& front = dq.front();
        std::uniform_int_distribution<std::uint64_t> ex(1, front.second);
        const qty_t n = ex(rng);
        encode_execute(feed.bytes, stock, ts++, front.first, n, next_match++);
        ++feed.executes;
        if (n >= front.second) {
            dq.pop_front();
            --live_count;
            if (dq.empty()) book.erase(it);
        } else {
            front.second -= n;
        }
    };

    // delete the front order of the inside level on one side.
    auto cancel_inside = [&](side resting_side) {
        auto& book = (resting_side == side::bid) ? bids : asks;
        auto  it   = (resting_side == side::bid) ? std::prev(book.end()) : book.begin();
        auto& dq   = it->second;
        const order_id_t id = dq.front().first;
        encode_delete(feed.bytes, stock, ts++, id);
        ++feed.deletes;
        dq.pop_front();
        --live_count;
        if (dq.empty()) book.erase(it);
    };

    for (std::size_t i = 0; i < message_count; ++i) {
        // always keep both sides populated so the book stays two-sided.
        if (bids.empty()) { seed_bid(); continue; }
        if (asks.empty()) { seed_ask(); continue; }

        const price_t bb   = best_bid();
        const price_t ba   = best_ask();
        const int     roll = action(rng);

        if (roll < 30) {  // join / deepen the bid (never crossing the ask)
            price_t px = bb - depth_dist(rng);
            if (px >= ba) px = ba - 1;
            if (px < lo) px = lo;
            if (px >= ba) { seed_ask(); continue; }  // no room: widen the ask side
            emit_add(side::bid, px);
        } else if (roll < 60) {  // join / deepen the ask
            price_t px = ba + depth_dist(rng);
            if (px <= bb) px = bb + 1;
            if (px > hi) px = hi;
            if (px <= bb) { seed_bid(); continue; }
            emit_add(side::ask, px);
        } else if (roll < 78) {  // buy aggressor lifts the inside ask
            aggress(side::ask);
        } else if (roll < 96) {  // sell aggressor hits the inside bid
            aggress(side::bid);
        } else {  // cancel at the touch
            cancel_inside((rng() & 1u) ? side::bid : side::ask);
        }
    }

    feed.messages      = message_count;
    feed.expected_live = live_count;
    return feed;
}

}  // namespace hft::itch
