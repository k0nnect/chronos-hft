// c++ co-simulation wrapper around the verilated feature_extractor model.
//
// FpgaFeatureEngine presents the rtl accelerator as an ordinary low-latency c++
// object: push a book update, pop a feature beat. it owns the verilated model,
// drives its clock, & models the structural pipeline latency so callers can
// reason about tick-to-feature delay exactly as on real hardware over pcie/axi.
//
// the hot methods (push/pop/tick) perform no allocation & do not throw; the
// only heap use is the verilated model constructed once at startup. this header
// is only included in the HFT_BUILD_HARDWARE configuration, where the verilator
// build has generated Vfeature_extractor.h.
#pragma once

#include <cstdint>

#include "hft/core/types.hpp"
#include "hft/engine/book_update.hpp"

class Vfeature_extractor;  // forward decl: defined by the verilator build

namespace hft::hw {

class FpgaFeatureEngine {
public:
    // FRAC + 3, matching feature_extractor's datapath latency.
    static constexpr unsigned kFrac    = 16;
    static constexpr unsigned kLatency = kFrac + 3;

    // base_price is subtracted from absolute book prices before they enter the
    // accelerator (which works in band-relative space to keep results in 32 bits).
    explicit FpgaFeatureEngine(price_t base_price = 0);
    ~FpgaFeatureEngine();

    FpgaFeatureEngine(const FpgaFeatureEngine&)            = delete;
    FpgaFeatureEngine& operator=(const FpgaFeatureEngine&) = delete;

    // hold reset for several cycles to flush the pipeline to a known state.
    void reset() noexcept;

    // present one book update on the inbound stream & advance one clock.
    // returns the slave tready (true == accepted). does not allocate / throw.
    bool push_book_tick(const book_update& update) noexcept;

    // advance one clock with no new input (drains in-flight beats).
    void idle_tick() noexcept;

    // read the outbound feature beat valid in the current cycle. returns false
    // when no feature is presented this cycle. micro_price is Q16.16 band-
    // relative; imbalance is signed Q.16.
    bool pop_feature_tick(std::uint32_t& out_micro_price, std::int32_t& out_imbalance) noexcept;

    [[nodiscard]] std::uint64_t cycles() const noexcept { return cycles_; }
    [[nodiscard]] price_t       base_price() const noexcept { return base_; }
    [[nodiscard]] static constexpr unsigned latency_cycles() noexcept { return kLatency; }

private:
    void tick_() noexcept;  // one rising-edge clock with current inputs

    Vfeature_extractor* top_;
    std::uint64_t       cycles_ = 0;
    price_t             base_;
};

}  // namespace hft::hw
