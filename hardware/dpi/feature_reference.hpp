// bit-exact software model of the feature_extractor datapath.
//
// this is the single source of truth for the fixed-point arithmetic: the rtl is
// written to reproduce it exactly, & the cosimulation asserts
// rtl_output == feature_reference(...) bit-for-bit. keeping it as a tiny header
// also lets the numeric design be validated against the floating-point
// order_book reference without a verilator toolchain present.
//
// formats (FRAC = 16):
//   micro_price : unsigned Q16.16, band-relative  (== round-toward-zero of the
//                 hardware accumulate, matching the rtl's truncation)
//   imbalance   : signed   Q.16,   value == imbalance * 2^16
#pragma once

#include <cstdint>

namespace hft::hw {

inline constexpr unsigned kFeatureFrac = 16;

struct feature_fixed {
    std::uint32_t micro_price;  // Q16.16, band-relative
    std::int32_t  imbalance;    // Q.16 signed
};

// inputs are band-relative prices (small) & raw quantities, exactly the words
// placed on the inbound axi beat. denom must be non-zero (a two-sided book).
[[nodiscard]] inline feature_fixed compute_features_fixed(std::uint32_t bid_price_rel,
                                                          std::uint32_t ask_price_rel,
                                                          std::uint32_t bid_qty,
                                                          std::uint32_t ask_qty) noexcept {
    const std::uint64_t denom = static_cast<std::uint64_t>(bid_qty) + ask_qty;

    // weight = floor(bid_qty * 2^FRAC / denom)  (radix-2 fractional divider).
    const std::uint64_t weight =
        (static_cast<std::uint64_t>(bid_qty) << kFeatureFrac) / denom;

    // micro = (bid << FRAC) + (ask - bid) * weight, truncated to 32 bits.
    const std::int64_t spread =
        static_cast<std::int64_t>(ask_price_rel) - static_cast<std::int64_t>(bid_price_rel);
    const std::int64_t micro =
        (static_cast<std::int64_t>(bid_price_rel) << kFeatureFrac) +
        spread * static_cast<std::int64_t>(weight);

    // imbalance = sign * floor(|bid_qty - ask_qty| * 2^FRAC / denom).
    const bool          neg     = ask_qty > bid_qty;
    const std::uint64_t absdiff = neg ? (static_cast<std::uint64_t>(ask_qty) - bid_qty)
                                      : (static_cast<std::uint64_t>(bid_qty) - ask_qty);
    const std::uint64_t ratio   = (absdiff << kFeatureFrac) / denom;
    const std::int32_t  imb     = neg ? -static_cast<std::int32_t>(ratio)
                                      :  static_cast<std::int32_t>(ratio);

    return feature_fixed{static_cast<std::uint32_t>(static_cast<std::uint64_t>(micro) & 0xFFFFFFFFull),
                         imb};
}

}  // namespace hft::hw
