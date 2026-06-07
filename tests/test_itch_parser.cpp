// round-trips every message type through encode -> frame_cursor -> decode and
// checks the normalized fields, plus framing edge cases: multiple frames in one
// buffer, a truncated trailing frame, and a malformed (bad-type) frame.
#include <cstdint>
#include <vector>

#include "check.hpp"
#include "hft/feed/itch_generator.hpp"
#include "hft/feed/itch_parser.hpp"

using namespace hft;

namespace {

constexpr price_t kBase = 1'000'000;

// decode exactly one frame from a freshly built buffer.
bool decode_one(const std::vector<std::uint8_t>& buf, market_event& ev) {
    itch::frame_cursor cursor(buf.data(), buf.size());
    std::uint16_t      len = 0;
    const std::uint8_t* body = cursor.next(len);
    check(body != nullptr);
    return itch::decode(body, len, ev);
}

void add_round_trip() {
    std::vector<std::uint8_t> buf;
    itch::encode_add(buf, /*stock=*/7, /*ts=*/123456789, /*ref=*/4242, side::ask,
                     /*shares=*/500, kBase + 100);
    market_event ev;
    check(decode_one(buf, ev));
    check(ev.type == event_type::add);
    check_eq(ev.stock_locate, 7u);
    check_eq(ev.timestamp, 123456789u);
    check_eq(ev.order_id, 4242u);
    check(ev.s == side::ask);
    check_eq(ev.qty, 500u);
    check_eq(ev.price, kBase + 100);
}

void execute_round_trip() {
    std::vector<std::uint8_t> buf;
    itch::encode_execute(buf, 1, 9000, /*ref=*/55, /*executed=*/120, /*match=*/777);
    market_event ev;
    check(decode_one(buf, ev));
    check(ev.type == event_type::execute);
    check_eq(ev.order_id, 55u);
    check_eq(ev.qty, 120u);
    check_eq(ev.match_number, 777u);
}

void cancel_delete_round_trip() {
    std::vector<std::uint8_t> buf;
    itch::encode_cancel(buf, 1, 1, /*ref=*/9, /*cancelled=*/30);
    market_event ev;
    check(decode_one(buf, ev));
    check(ev.type == event_type::reduce);
    check_eq(ev.order_id, 9u);
    check_eq(ev.qty, 30u);

    buf.clear();
    itch::encode_delete(buf, 1, 2, /*ref=*/9);
    check(decode_one(buf, ev));
    check(ev.type == event_type::delete_order);
    check_eq(ev.order_id, 9u);
}

void replace_round_trip() {
    std::vector<std::uint8_t> buf;
    itch::encode_replace(buf, 1, 3, /*orig=*/100, /*fresh=*/200, /*shares=*/640, kBase + 5);
    market_event ev;
    check(decode_one(buf, ev));
    check(ev.type == event_type::replace);
    check_eq(ev.order_id, 100u);
    check_eq(ev.new_order_id, 200u);
    check_eq(ev.qty, 640u);
    check_eq(ev.price, kBase + 5);
}

void trade_round_trip() {
    std::vector<std::uint8_t> buf;
    itch::encode_trade(buf, 1, 4, /*ref=*/11, side::bid, /*shares=*/75, kBase + 3, /*match=*/9);
    market_event ev;
    check(decode_one(buf, ev));
    check(ev.type == event_type::trade);
    check(ev.s == side::bid);
    check_eq(ev.qty, 75u);
    check_eq(ev.price, kBase + 3);
    check_eq(ev.match_number, 9u);
}

void multiple_frames_in_one_buffer() {
    std::vector<std::uint8_t> buf;
    itch::encode_add(buf, 1, 1, 1, side::bid, 100, kBase);
    itch::encode_add(buf, 1, 2, 2, side::ask, 200, kBase + 2);
    itch::encode_delete(buf, 1, 3, 1);

    itch::frame_cursor cursor(buf.data(), buf.size());
    market_event       ev;
    std::uint16_t      len = 0;
    int                seen = 0;
    for (const std::uint8_t* body = cursor.next(len); body; body = cursor.next(len)) {
        check(itch::decode(body, len, ev));
        ++seen;
    }
    check_eq(seen, 3);
    check(cursor.exhausted());
    check_eq(cursor.consumed(), buf.size());
}

void truncated_trailing_frame_is_left_unconsumed() {
    std::vector<std::uint8_t> buf;
    itch::encode_add(buf, 1, 1, 1, side::bid, 100, kBase);
    const std::size_t whole = buf.size();
    itch::encode_add(buf, 1, 2, 2, side::ask, 200, kBase + 2);
    // chop the second frame in half: the cursor must stop cleanly after the first.
    buf.resize(whole + 5);

    itch::frame_cursor cursor(buf.data(), buf.size());
    market_event       ev;
    std::uint16_t      len  = 0;
    int                seen = 0;
    for (const std::uint8_t* body = cursor.next(len); body; body = cursor.next(len)) {
        check(itch::decode(body, len, ev));
        ++seen;
    }
    check_eq(seen, 1);
    check(!cursor.exhausted());
    check_eq(cursor.consumed(), whole);  // only the first frame was consumed
}

void malformed_type_is_rejected() {
    // hand-build a 1-byte frame carrying an unknown message type 'Z'.
    std::vector<std::uint8_t> buf;
    itch::frame_writer        w(buf);
    w.ch('Z');
    market_event ev;
    itch::frame_cursor cursor(buf.data(), buf.size());
    std::uint16_t      len  = 0;
    const std::uint8_t* body = cursor.next(len);
    check(body != nullptr);   // framing is valid...
    check(!itch::decode(body, len, ev));  // ...but the payload is not decodable
}

}  // namespace

int main() {
    run_suite(add_round_trip);
    run_suite(execute_round_trip);
    run_suite(cancel_delete_round_trip);
    run_suite(replace_round_trip);
    run_suite(trade_round_trip);
    run_suite(multiple_frames_in_one_buffer);
    run_suite(truncated_trailing_frame_is_left_unconsumed);
    run_suite(malformed_type_is_rejected);
    return hft_test_summary("itch_parser");
}
