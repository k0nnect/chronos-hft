// minimal test harness: a check macro and a tiny runner, no dependencies.
#pragma once

#include <cstdio>
#include <cstdlib>

namespace hfttest {

inline int g_failures = 0;

}  // namespace hfttest

// records and reports a failed expectation but keeps going so one run surfaces
// every problem at once.
#define check(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "  FAIL %s:%d  check(%s)\n", __FILE__,        \
                         __LINE__, #cond);                                     \
            ++hfttest::g_failures;                                             \
        }                                                                      \
    } while (0)

#define check_eq(a, b)                                                         \
    do {                                                                       \
        const auto _va = (a);                                                  \
        const auto _vb = (b);                                                  \
        if (!(_va == _vb)) {                                                   \
            std::fprintf(stderr,                                               \
                         "  FAIL %s:%d  check_eq(%s, %s)  lhs=%lld rhs=%lld\n", \
                         __FILE__, __LINE__, #a, #b,                           \
                         (long long)(_va), (long long)(_vb));                  \
            ++hfttest::g_failures;                                             \
        }                                                                      \
    } while (0)

#define run_suite(fn)                                                         \
    do {                                                                      \
        std::printf("[ run ] %s\n", #fn);                                     \
        fn();                                                                 \
    } while (0)

inline int hft_test_summary(const char* suite) {
    if (hfttest::g_failures == 0) {
        std::printf("[ ok  ] %s: all checks passed\n", suite);
        return 0;
    }
    std::fprintf(stderr, "[fail ] %s: %d check(s) failed\n", suite, hfttest::g_failures);
    return 1;
}
