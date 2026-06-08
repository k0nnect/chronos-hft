// append-only jsonl trace logger for the replay dashboard.
//
// one json object per line (jsonl): cheap to append while a backtest runs &
// trivially streamable on the frontend, which reads the file line by line. the
// engine calls on_tick() once per event; the logger emits a snapshot only when
// simulation time crosses the next interval boundary (100 ms by default), so the
// file stays small regardless of event rate.
//
// every snapshot carries: the timestamp, best bid/ask, the top `Depth` levels of
// each side as [price, volume] pairs, the current signed position & realized p&l.
//
// the hot path is allocation-free: formatting goes into a fixed member buffer via
// hand-rolled integer conversion (no streams, no std::to_string), then a single
// fwrite. the only heap touch is whatever the C library does inside fopen at
// open() time, which is cold setup, never the per-event path.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "hft/core/compiler.hpp"
#include "hft/core/types.hpp"

namespace hft {

// production cadence: one snapshot per 100 ms of simulation time (timestamps are
// nanoseconds). callers replaying a synthetic feed whose stamps are not real
// nanoseconds can dial this down with set_interval().
inline constexpr ts_t default_trace_interval_ns = 100'000'000;

template <std::size_t Depth = 5, std::size_t BufBytes = 4096>
class trace_logger {
    static_assert(Depth > 0, "need at least one book level");
    static_assert(BufBytes >= 256, "line buffer too small to be useful");

public:
    trace_logger() noexcept = default;
    explicit trace_logger(ts_t interval_ns) noexcept
        : interval_ns_(interval_ns != 0 ? interval_ns : 1) {}
    ~trace_logger() noexcept { close(); }

    // owns a FILE*; copying would double-close it.
    trace_logger(const trace_logger&)            = delete;
    trace_logger& operator=(const trace_logger&) = delete;

    // open (truncating) the trace file. returns false if it cannot be created.
    [[nodiscard]] bool open(const char* path) noexcept {
        close();
        fp_    = std::fopen(path, "wb");
        armed_ = false;
        lines_ = 0;
        return fp_ != nullptr;
    }

    // flush & release the file. idempotent; also run by the destructor.
    void close() noexcept {
        if (fp_ != nullptr) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

    [[nodiscard]] bool          is_open() const noexcept { return fp_ != nullptr; }
    [[nodiscard]] std::uint64_t lines() const noexcept { return lines_; }
    void set_interval(ts_t interval_ns) noexcept {
        interval_ns_ = interval_ns != 0 ? interval_ns : 1;
    }

    // engine hook, called every event. emits at most one snapshot per call, when
    // `now` reaches the next interval boundary. a no-op (one branch) when closed.
    template <typename Book>
    hft_hot void on_tick(ts_t now, const Book& book, std::int64_t position,
                         double realized_pnl) noexcept {
        if (fp_ == nullptr) {
            return;
        }
        if (!armed_) [[unlikely]] {
            armed_   = true;
            next_ts_ = now;  // first event arms the schedule at its own timestamp
        }
        if (now < next_ts_) {
            return;
        }
        write_snapshot(now, book, position, realized_pnl);
        do {
            next_ts_ += interval_ns_;  // advance past `now`, skipping idle gaps
        } while (next_ts_ <= now);
    }

    // format & append one snapshot line unconditionally (ignores the cadence).
    // exposed for tests & ad-hoc callers; the engine path goes through on_tick.
    template <typename Book>
    void write_snapshot(ts_t now, const Book& book, std::int64_t position,
                        double realized_pnl) noexcept {
        if (fp_ == nullptr) {
            return;
        }
        len_ = 0;
        put('{');
        key("t");    u64(now);
        sep(); key("bb");   if (book.has_bid()) i64(book.best_bid()); else lit("null");
        sep(); key("ba");   if (book.has_ask()) i64(book.best_ask()); else lit("null");
        sep(); key("bids"); levels(book, side::bid);
        sep(); key("asks"); levels(book, side::ask);
        sep(); key("pos");  i64(position);
        sep(); key("pnl");  dbl(realized_pnl);
        put('}');
        put('\n');
        std::fwrite(buf_, 1, len_, fp_);
        ++lines_;
    }

private:
    // ---- fixed-buffer json appenders (no allocation, bounds-checked) --------

    hft_always_inline void put(char c) noexcept {
        if (len_ < BufBytes) {
            buf_[len_++] = c;
        }
    }
    hft_always_inline void lit(const char* s) noexcept {
        while (*s != '\0') {
            put(*s++);
        }
    }
    hft_always_inline void sep() noexcept { put(','); }
    hft_always_inline void key(const char* k) noexcept {
        put('"');
        lit(k);
        put('"');
        put(':');
    }

    void u64(std::uint64_t v) noexcept {
        if (v == 0) {
            put('0');
            return;
        }
        char        tmp[20];  // 2^64 - 1 is 20 decimal digits
        std::size_t n = 0;
        while (v != 0) {
            tmp[n++] = static_cast<char>('0' + static_cast<int>(v % 10));
            v /= 10;
        }
        while (n != 0) {
            put(tmp[--n]);
        }
    }

    void i64(std::int64_t v) noexcept {
        if (v < 0) {
            put('-');
            // negate via unsigned to stay well-defined even at INT64_MIN.
            u64(static_cast<std::uint64_t>(-(v + 1)) + 1u);
        } else {
            u64(static_cast<std::uint64_t>(v));
        }
    }

    void dbl(double v) noexcept {
        if (len_ >= BufBytes) {
            return;
        }
        const std::size_t room = BufBytes - len_;
        const int written = std::snprintf(buf_ + len_, room, "%.4f", v);
        if (written > 0) {
            const std::size_t w = static_cast<std::size_t>(written);
            len_ += (w < room) ? w : (room - 1);  // snprintf truncates; never overrun
        }
    }

    // emit one side's top levels as a json array of [price, volume] pairs.
    template <typename Book>
    void levels(const Book& book, side s) noexcept {
        put('[');
        bool first = true;
        book.walk(s, Depth, [&](price_t price, qty_t qty) noexcept -> bool {
            if (!first) {
                sep();
            }
            first = false;
            put('[');
            i64(price);
            sep();
            u64(qty);
            put(']');
            return true;  // walk already stops after Depth populated levels
        });
        put(']');
    }

    std::FILE*    fp_          = nullptr;
    ts_t          interval_ns_ = default_trace_interval_ns;
    ts_t          next_ts_     = 0;
    bool          armed_       = false;
    std::uint64_t lines_       = 0;
    std::size_t   len_         = 0;
    char          buf_[BufBytes];
};

}  // namespace hft
