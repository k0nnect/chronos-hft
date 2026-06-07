cmake_minimum_required(VERSION 3.20)

project(hft-bt
  VERSION 0.1.0
  DESCRIPTION "hardware-accelerated high-frequency trading backtester"
  LANGUAGES CXX)

# default to a release build (the configuration the project is tuned for) when
# the caller has not picked one explicitly.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(compilerflags)

# the core book/data-structure layer is header-only; expose it as an interface
# library that carries the public include path and the shared flags.
add_library(hft_core INTERFACE)
target_include_directories(hft_core INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}")  # so "hardware/dpi/feature_reference.hpp" resolves
target_link_libraries(hft_core INTERFACE hft_flags)

find_package(Threads REQUIRED)

# phase 1 demo / benchmark driver.
add_executable(hft_demo src/main.cpp)
target_link_libraries(hft_demo PRIVATE hft_core)

# phase 3 backtest driver: the micro-price market maker over the full pipeline.
add_executable(micro_price_mm src/strategies/micro_price_mm.cpp)
target_link_libraries(micro_price_mm PRIVATE hft_core Threads::Threads)

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)

# ---------------------------------------------------------------------------
# phase 4: verilator hardware acceleration layer (opt-in).
#
# enable with -DHFT_BUILD_HARDWARE=ON. requires a verilator installation
# (set VERILATOR_ROOT or have `verilator` on PATH). when on, the systemverilog
# rtl is verilated and linked into the cosimulation cross-check and the hardware
# benchmark.
# ---------------------------------------------------------------------------
option(HFT_BUILD_HARDWARE "build the verilator hardware acceleration layer" OFF)

if(HFT_BUILD_HARDWARE)
  find_package(verilator HINTS $ENV{VERILATOR_ROOT})
  if(NOT verilator_FOUND)
    message(FATAL_ERROR
      "HFT_BUILD_HARDWARE=ON but Verilator was not found. "
      "Install verilator and/or set VERILATOR_ROOT.")
  endif()
  find_program(VERILATOR_EXE NAMES verilator HINTS $ENV{VERILATOR_ROOT}/bin)

  # rtl compiled into the verilated model. order matters: leaf modules before the
  # top. the flat-port top does not use axi_stream_if.sv / hft_axi_pkg (those wire
  # the systemverilog testbench), so they are not fed to the top's verilation.
  set(HFT_RTL
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/frac_divider.sv"
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/micro_price.sv"
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/volume_imbalance.sv"
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/feature_extractor.sv")

  set(HFT_VERILATOR_ARGS -Wall -Wno-fatal --x-assign fast --x-initial fast -O3)

  # cosimulation correctness cross-check (registered with ctest).
  add_executable(hardware_cosim
    hardware/tb/cosim_check.cpp
    hardware/dpi/fpga_dpi.cpp)
  target_link_libraries(hardware_cosim PRIVATE hft_core)
  verilate(hardware_cosim
    SOURCES ${HFT_RTL}
    TOP_MODULE feature_extractor
    PREFIX Vfeature_extractor
    VERILATOR_ARGS ${HFT_VERILATOR_ARGS})
  add_test(NAME hardware_cosim COMMAND hardware_cosim)

  # hardware vs software performance benchmark.
  add_executable(bench_hardware
    bench/bench_hardware.cpp
    hardware/dpi/fpga_dpi.cpp)
  target_link_libraries(bench_hardware PRIVATE hft_core)
  verilate(bench_hardware
    SOURCES ${HFT_RTL}
    TOP_MODULE feature_extractor
    PREFIX Vfeature_extractor
    VERILATOR_ARGS ${HFT_VERILATOR_ARGS})

  # lint-only target for the rtl.
  if(VERILATOR_EXE)
    add_custom_target(hardware_lint
      COMMAND ${VERILATOR_EXE} --lint-only -Wall --top-module feature_extractor ${HFT_RTL}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "linting systemverilog rtl with verilator")
  endif()

  message(STATUS "hft: hardware acceleration layer enabled (verilator found)")
endif()
