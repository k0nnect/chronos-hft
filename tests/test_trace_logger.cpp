// tests for the jsonl trace logger: the 100 ms-style cadence throttling, the
// exact json a snapshot produces (best bid/ask, top-5 levels, position, pnl), &
// the empty-book edge case. each test writes to a temp file & reads it back as
// text, asserting on the bytes the dashboard will actually parse.
#include <cstddef>
#include <cstdio>
#include <string>

#include <unistd.h>

#include "check.hpp"
#include "hft/book/order_book.hpp"
#include "hft/metrics/trace_logger.hpp"

using namespace hft;

namespace {

using book_t = order_book<8192, 1u << 16>;
constexpr price_t kBase = 1'000'000;

std::string scratch_path() {
    return std::string("/tmp/chronos_trace_test_") + std::to_string(::getpid()) + ".jsonl";
}

// read an entire file into a string (tests may allocate freely).
std::string slurp(const std::string& path) {
    std::string out;
    std::FILE*  f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return out;
    }
    char        chunk[4096];
    std::size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        out.append(chunk, n);
    }
    std::fclose(f);
    return out;
}

std::size_t count_lines(const std::string& s) {
    std::size_t n = 0;
    for (char c : s) {
        n += (c == '\n') ? 1u : 0u;
    }
    return n;
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// a two-sided book: bids 50@+100, 70@+99 ; asks 60@+102, 40@+103. order_book is
// non-movable (it owns the level arrays), so populate one in place.
void fill_book(book_t& b) {
    b.add(1, side::bid, kBase + 100, 50);
    b.add(2, side::bid, kBase + 99, 70);
    b.add(3, side::ask, kBase + 102, 60);
    b.add(4, side::ask, kBase + 103, 40);
}

// on_tick emits only when sim time crosses the interval boundary.
void cadence_throttles_to_interval() {
    book_t book(kBase);
    fill_book(book);
    const std::string path = scratch_path();

    trace_logger<> tracer(/*interval_ns=*/100);
    check(tracer.open(path.c_str()));

    tracer.on_tick(0, book, 0, 0.0);     // arms at 0, emits (boundary 0)
    check_eq(tracer.lines(), 1u);
    tracer.on_tick(50, book, 0, 0.0);    // 50 < 100 -> no emit
    check_eq(tracer.lines(), 1u);
    tracer.on_tick(100, book, 0, 0.0);   // boundary 100 -> emit
    check_eq(tracer.lines(), 2u);
    tracer.on_tick(150, book, 0, 0.0);   // 150 < 200 -> no emit
    check_eq(tracer.lines(), 2u);
    tracer.on_tick(250, book, 0, 0.0);   // crosses 200 (skips idle gap) -> emit once
    check_eq(tracer.lines(), 3u);

    tracer.close();
    const std::string text = slurp(path);
    check_eq(count_lines(text), 3u);
    check(text.front() == '{');
    std::remove(path.c_str());
}

// a snapshot serializes every required field with the expected shape.
void snapshot_serializes_all_fields() {
    book_t book(kBase);
    fill_book(book);
    const std::string path = scratch_path();

    trace_logger<> tracer;
    check(tracer.open(path.c_str()));
    tracer.write_snapshot(/*now=*/12345, book, /*position=*/-50, /*realized_pnl=*/1234.5);
    tracer.close();

    const std::string line = slurp(path);
    check_eq(count_lines(line), 1u);

    check(contains(line, "\"t\":12345"));
    check(contains(line, "\"bb\":1000100"));         // best bid price
    check(contains(line, "\"ba\":1000102"));         // best ask price
    check(contains(line, "\"pos\":-50"));            // signed position
    check(contains(line, "\"pnl\":1234.5000"));      // %.4f formatting

    // top levels, inside-out, as [price, volume] pairs.
    check(contains(line, "\"bids\":[[1000100,50],[1000099,70]]"));
    check(contains(line, "\"asks\":[[1000102,60],[1000103,40]]"));
    std::remove(path.c_str());
}

// an empty book yields null touches & empty level arrays (still valid json).
void empty_book_is_valid_json() {
    const book_t book(kBase);  // no orders
    const std::string path = scratch_path();

    trace_logger<> tracer;
    check(tracer.open(path.c_str()));
    tracer.write_snapshot(/*now=*/7, book, /*position=*/0, /*realized_pnl=*/0.0);
    tracer.close();

    const std::string line = slurp(path);
    check(contains(line, "\"bb\":null"));
    check(contains(line, "\"ba\":null"));
    check(contains(line, "\"bids\":[]"));
    check(contains(line, "\"asks\":[]"));
    check(contains(line, "\"pnl\":0.0000"));
    std::remove(path.c_str());
}

}  // namespace

int main() {
    run_suite(cadence_throttles_to_interval);
    run_suite(snapshot_serializes_all_fields);
    run_suite(empty_book_is_valid_json);
    return hft_test_summary("trace_logger");
}
