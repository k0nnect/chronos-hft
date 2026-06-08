# self-contained assert-based tests (no external test framework dependency so
# the project builds and verifies with nothing but a c++20 compiler + cmake).
# each test is an executable that returns non-zero on failure; ctest runs them.

find_package(Threads REQUIRED)

function(hft_add_test name)
  add_executable(${name} ${name}.cpp)
  # any extra args are additional libraries this test links against.
  target_link_libraries(${name} PRIVATE hft_core Threads::Threads ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# phase 1
hft_add_test(test_memory_pool)
hft_add_test(test_order_index_map)
hft_add_test(test_order_book)

# phase 2
hft_add_test(test_spsc_ring)
hft_add_test(test_itch_parser)
hft_add_test(test_feed_integration)

# phase 3
hft_add_test(test_engine)
hft_add_test(test_market_impact)
hft_add_test(test_trace_logger)

# phase 5 -- offline pcap replay (links the mmap reader implementation).
hft_add_test(test_pcap hft_feed)

# phase 6 -- statistical maker: alpha engine (header-only) & the quoting strategy.
hft_add_test(test_alpha_engine)
hft_add_test(test_statistical_mm hft_stat_mm)

# phase 4 -- the fixed-point feature reference the rtl mirrors (no verilator
# needed; the verilated cosim cross-check lives behind HFT_BUILD_HARDWARE).
hft_add_test(test_feature_golden)
