// network byte-order (big-endian) load/store helpers.
//
// exchange binary protocols (itch/ouch and friends) are big-endian on the wire.
// these helpers do a single unaligned memcpy plus a byteswap that the compiler
// lowers to one `bswap`/`rev` instruction, so decoding a field is branch-free
// and never depends on the buffer being aligned. host endianness is resolved at
// compile time via std::endian, so the little-endian path costs nothing on x86.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace hft {

[[nodiscard]] constexpr std::uint16_t bswap(std::uint16_t v) noexcept {
    return static_cast<std::uint16_t>((v << 8) | (v >> 8));
}

[[nodiscard]] constexpr std::uint32_t bswap(std::uint32_t v) noexcept {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

[[nodiscard]] constexpr std::uint64_t bswap(std::uint64_t v) noexcept {
    return ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) |
           ((v & 0x0000000000FF0000ull) << 24) | ((v & 0x00000000FF000000ull) << 8) |
           ((v & 0x000000FF00000000ull) >> 8) | ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
}

// read a big-endian unsigned integer of type T from raw bytes.
template <typename T>
[[nodiscard]] inline T load_be(const void* src) noexcept {
    static_assert(std::is_unsigned_v<T>, "load_be expects an unsigned integer type");
    T v;
    std::memcpy(&v, src, sizeof(T));
    if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::little) {
        v = bswap(v);
    }
    return v;
}

// write value as a big-endian unsigned integer of type T to raw bytes.
template <typename T>
inline void store_be(void* dst, T v) noexcept {
    static_assert(std::is_unsigned_v<T>, "store_be expects an unsigned integer type");
    if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::little) {
        v = bswap(v);
    }
    std::memcpy(dst, &v, sizeof(T));
}

}  // namespace hft
