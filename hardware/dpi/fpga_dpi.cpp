// implementation of the verilated feature_extractor co-simulation wrapper.
//
// drives the model with an explicit two-phase clock (clk low/eval, clk
// high/eval) and packs/unpacks the axi beats exactly as hft_axi_pkg defines
// them. the inbound 128-bit beat is four little-endian 32-bit words; verilator
// exposes it as a wide signal indexable word-by-word. the outbound 64-bit beat
// is a single 64-bit word with micro_price in the low half and imbalance in the
// high half.
#include "hardware/dpi/fpga_dpi.hpp"

#include "Vfeature_extractor.h"

namespace hft::hw {

FpgaFeatureEngine::FpgaFeatureEngine(price_t base_price)
    : top_(new Vfeature_extractor), base_(base_price) {
    top_->clk      = 0;
    top_->rst_n    = 0;
    top_->s_tvalid = 0;
    top_->s_tlast  = 0;
    top_->m_tready = 1;  // always-ready downstream
    top_->s_tdata[0] = 0;
    top_->s_tdata[1] = 0;
    top_->s_tdata[2] = 0;
    top_->s_tdata[3] = 0;
    top_->eval();
}

FpgaFeatureEngine::~FpgaFeatureEngine() {
    top_->final();
    delete top_;
}

void FpgaFeatureEngine::tick_() noexcept {
    top_->clk = 0;
    top_->eval();
    top_->clk = 1;
    top_->eval();
    ++cycles_;
}

void FpgaFeatureEngine::reset() noexcept {
    top_->rst_n    = 0;
    top_->s_tvalid = 0;
    top_->s_tlast  = 0;
    top_->m_tready = 1;
    for (unsigned i = 0; i < 4; ++i) {
        tick_();
    }
    top_->rst_n = 1;
    top_->eval();
}

bool FpgaFeatureEngine::push_book_tick(const book_update& update) noexcept {
    // band-relative prices keep the Q16.16 micro-price inside 32 bits.
    const std::uint32_t bid_rel = static_cast<std::uint32_t>(update.best_bid - base_);
    const std::uint32_t ask_rel = static_cast<std::uint32_t>(update.best_ask - base_);

    // pack book_update_beat_t: [31:0]=bid_price, [63:32]=bid_qty,
    // [95:64]=ask_price, [127:96]=ask_qty.
    top_->s_tdata[0] = bid_rel;
    top_->s_tdata[1] = static_cast<std::uint32_t>(update.bid_qty);
    top_->s_tdata[2] = ask_rel;
    top_->s_tdata[3] = static_cast<std::uint32_t>(update.ask_qty);
    top_->s_tvalid   = 1;
    top_->s_tlast    = 0;

    tick_();
    return top_->s_tready != 0;
}

void FpgaFeatureEngine::idle_tick() noexcept {
    top_->s_tvalid = 0;
    top_->s_tlast  = 0;
    tick_();
}

bool FpgaFeatureEngine::pop_feature_tick(std::uint32_t& out_micro_price,
                                         std::int32_t& out_imbalance) noexcept {
    if (top_->m_tvalid == 0) {
        return false;
    }
    const std::uint64_t beat = static_cast<std::uint64_t>(top_->m_tdata);
    out_micro_price = static_cast<std::uint32_t>(beat & 0xFFFFFFFFull);
    out_imbalance   = static_cast<std::int32_t>(static_cast<std::uint32_t>(beat >> 32));
    return true;
}

}  // namespace hft::hw
