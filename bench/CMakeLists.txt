# standalone micro-benchmarks. these are throwaway timing harnesses, not tests,
# so they are built but not registered with ctest.
find_package(Threads REQUIRED)

add_executable(bench_feed bench_feed.cpp)
target_link_libraries(bench_feed PRIVATE hft_core Threads::Threads)
