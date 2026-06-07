// compiler-specific intrinsics, branch hints and inlining controls.
// these are the low-level knobs the hot path relies on; everything here
// degrades gracefully to a no-op on compilers that do not support it.
#pragma once

#if defined(__GNUC__) || defined(__clang__)
  #define hft_always_inline inline __attribute__((always_inline))
  #define hft_noinline      __attribute__((noinline))
  #define hft_hot           __attribute__((hot))
  #define hft_cold          __attribute__((cold))
  #define hft_restrict      __restrict__
  #define hft_pure          __attribute__((pure))
  #define hft_flatten       __attribute__((flatten))
  // rw: 0 = read, 1 = write. locality: 0 (none) .. 3 (high temporal locality).
  #define hft_prefetch(addr, rw, locality) __builtin_prefetch((addr), (rw), (locality))
  #define hft_expect(expr, val)            __builtin_expect((expr), (val))
  #define hft_assume(expr)                                                       \
      do {                                                                       \
          if (!(expr)) __builtin_unreachable();                                  \
      } while (0)
#elif defined(_MSC_VER)
  #include <intrin.h>
  #define hft_always_inline __forceinline
  #define hft_noinline      __declspec(noinline)
  #define hft_hot
  #define hft_cold
  #define hft_restrict      __restrict
  #define hft_pure
  #define hft_flatten
  #define hft_prefetch(addr, rw, locality) _mm_prefetch((const char*)(addr), (locality))
  #define hft_expect(expr, val)            (expr)
  #define hft_assume(expr)                 __assume(expr)
#else
  #define hft_always_inline inline
  #define hft_noinline
  #define hft_hot
  #define hft_cold
  #define hft_restrict
  #define hft_pure
  #define hft_flatten
  #define hft_prefetch(addr, rw, locality) ((void)0)
  #define hft_expect(expr, val)            (expr)
  #define hft_assume(expr)                 ((void)0)
#endif
