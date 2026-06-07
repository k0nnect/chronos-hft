# centralised optimisation / warning flags applied to every target via the
# hft_flags interface library. release builds target the host micro-architecture
# and turn on the aggressive scalar/vector optimisations the hot path needs.

add_library(hft_flags INTERFACE)

target_compile_features(hft_flags INTERFACE cxx_std_20)

if(MSVC)
  target_compile_options(hft_flags INTERFACE
    /W4
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Release>:/Oi>
    $<$<CONFIG:Release>:/Ot>
    $<$<CONFIG:Release>:/GL>
  )
  target_link_options(hft_flags INTERFACE
    $<$<CONFIG:Release>:/LTCG>
  )
else()
  target_compile_options(hft_flags INTERFACE
    -Wall
    -Wextra
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wnon-virtual-dtor
    -Wdouble-promotion
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Release>:-mtune=native>
    $<$<CONFIG:Release>:-funroll-loops>
    $<$<CONFIG:Release>:-fomit-frame-pointer>
    $<$<CONFIG:Release>:-fno-plt>
    $<$<CONFIG:Release>:-fstrict-aliasing>
    $<$<CONFIG:RelWithDebInfo>:-O3>
    $<$<CONFIG:RelWithDebInfo>:-march=native>
    $<$<CONFIG:RelWithDebInfo>:-g>
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
  )
endif()

# enable link-time optimisation for release configs when the toolchain supports it.
include(CheckIPOSupported)
check_ipo_supported(RESULT _hft_ipo_ok OUTPUT _hft_ipo_msg)
if(_hft_ipo_ok)
  set_property(TARGET hft_flags PROPERTY INTERFACE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
