// cache-line constants and helpers used to align hot structures and to pad
// out fields that would otherwise share a line and cause false sharing.
#pragma once

#include <cstddef>

#if defined(__cpp_lib_hardware_interference_size) && __cpp_lib_hardware_interference_size
  #include <new>
#endif

namespace hft {

#if defined(__cpp_lib_hardware_interference_size) && __cpp_lib_hardware_interference_size
inline constexpr std::size_t cacheline_size = std::hardware_destructive_interference_size;
#else
// x86-64 / arm64 line size. fixed at 64 so the value is identical across
// translation units and matches the alignment baked into the abi here.
inline constexpr std::size_t cacheline_size = 64;
#endif

#define hft_cache_aligned alignas(::hft::cacheline_size)

// pads a struct out to the end of the current cache line. place an instance of
// padding<used_bytes> after fields that must not share a line with the next
// member (the classic single-producer / single-consumer false-sharing fix).
template <std::size_t UsedBytes>
struct padding {
    static_assert(UsedBytes < cacheline_size, "used bytes exceed one cache line");
    char bytes_[cacheline_size - UsedBytes];
};

template <>
struct padding<0> {
    char bytes_[cacheline_size];
};

}  // namespace hft
