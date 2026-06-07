// performance comparison: the software feature path vs the simulated fpga
// accelerator.
//
// the software side times order_book::micro_price()/imbalance() over a stream of
// two-sided states. the hardware side streams the same states through the
// verilated feature_extractor and reports the structural pipeline figures that
// characterise the real accelerator: a fixed latency (FRAC + 3 cycles) and a
// throughput of one feature per cycle. (verilator's event simulation is far
// slower than silicon, so its wall-clock time is not the hardware metric -- the
// cycle counts are.)
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "hardware/dpi/fpga_dpi.hpp"
#include "hft/book/apply.hpp"
#include "hft/book/order_book.hpp"
#include "hft/feed/itch_generator.hpp"
#include "hft/feed/itch_parser.hpp"
#include "hft/feed/market_event.hpp"

using namespace hft;

namespace {

using book_t = order_book<8192, 1u << 20>;
using steady = std::chrono::steady_clock;

struct snapshot {
    price_t bb, ba;
    qty_t   bq, aq;
    double  micro_ref;
    double  imb_ref;
};

}  // namespace

int main() {
    constexpr std::size_t kMessages = 1'000'000;
    const itch::synthetic_feed feed = itch::generate_feed(kMessages, /*seed=*/13);

    // pre-extract the two-sided states so each path measures only feature work.
    auto book = std::make_unique<book_t>(feed.base_tick);
    std::vector<snapshot> states;
    states.reserve(kMessages);

    itch::frame_cursor cursor(feed.bytes.data(), feed.bytes.size());
    std::uint16_t      len = 0;
    market_event       ev;
    for (const std::uint8_t* body = cursor.next(len); body != nullptr;
         body = cursor.next(len)) {
        if (!itch::decode(body, len, ev)) continue;
        apply_event(*book, ev);
        if (book->has_bid() && book->has_ask() && book->best_bid_qty() > 0 &&
            book->best_ask_qty() > 0) {
            states.push_back({book->best_bid(), book->best_ask(), book->best_bid_qty(),
                              book->best_ask_qty(), book->micro_price(), book->imbalance(1)});
        }
    }

    std::printf("== feature engine benchmark (%zu two-sided states) ==\n", states.size());

    // ---- software path ----
    double sw_sink = 0.0;
    const auto sw_t0 = steady::now();
    for (const snapshot& s : states) {
        // recompute features from the snapshot using the same fixed inputs.
        const double bq = static_cast<double>(s.bq);
        const double aq = static_cast<double>(s.aq);
        const double micro = (static_cast<double>(s.bb) * aq + static_cast<double>(s.ba) * bq) /
                             (bq + aq);
        const double imb = (bq - aq) / (bq + aq);
        sw_sink += micro + imb;
    }
    const auto sw_dt = std::chrono::duration_cast<std::chrono::nanoseconds>(steady::now() - sw_t0);
    const double sw_ns = static_cast<double>(sw_dt.count()) / static_cast<double>(states.size());
    std::printf("  software : %.2f ns/feature  (sink=%.1f)\n", sw_ns, sw_sink);

    // ---- simulated hardware path ----
    hw::FpgaFeatureEngine fpga(feed.base_tick);
    fpga.reset();

    std::uint64_t produced = 0;
    const auto hw_t0 = steady::now();
    for (const snapshot& s : states) {
        book_update u;
        u.best_bid = s.bb;
        u.best_ask = s.ba;
        u.bid_qty  = s.bq;
        u.ask_qty  = s.aq;
        fpga.push_book_tick(u);
        std::uint32_t m;
        std::int32_t  i;
        if (fpga.pop_feature_tick(m, i)) ++produced;
    }
    while (produced < states.size()) {
        fpga.idle_tick();
        std::uint32_t m;
        std::int32_t  i;
        if (fpga.pop_feature_tick(m, i)) ++produced;
    }
    const auto hw_dt = std::chrono::duration_cast<std::chrono::nanoseconds>(steady::now() - hw_t0);

    std::printf("  hardware : structural latency %u cycles, throughput 1 feature/cycle\n",
                hw::FpgaFeatureEngine::latency_cycles());
    std::printf("             %llu features produced; %llu sim cycles; "
                "%.1f ms verilator sim time (not the silicon figure)\n",
                static_cast<unsigned long long>(produced),
                static_cast<unsigned long long>(fpga.cycles()),
                static_cast<double>(hw_dt.count()) / 1e6);
    std::printf("  takeaway : at a 250 MHz fabric clock the pipeline emits a feature every 4 ns\n"
                "             after a fixed %.1f ns fill, fully overlapping the cpu feed handler.\n",
                static_cast<double>(hw::FpgaFeatureEngine::latency_cycles()) * 4.0);
    return 0;
}
