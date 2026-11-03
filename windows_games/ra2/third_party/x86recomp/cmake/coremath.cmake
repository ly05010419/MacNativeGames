# CORE-MATH (MIT): correctly rounded elementary functions, behind the x64
# CRT's math imports (src/runtime/hle/ucrt.cpp). Correct rounding makes the
# results the same on every host and compiler, which neither the host libm
# (glibc and Apple's differ) nor a port of the UCRT's own (not correctly
# rounded) would give; see ARCHITECTURE.md, "64-bit guests". Only the
# functions games import are built. On x86-64 a few of them take
# intrinsic-based fast paths for bit operations and flag handling; arm64
# takes their portable branches, with the same results.

FetchContent_Declare(coremath
  GIT_REPOSITORY https://gitlab.inria.fr/core-math/core-math.git
  GIT_TAG 74fcd1094a316164c94481f40f5c9658439bee63)  # 2026-10-08
FetchContent_MakeAvailable(coremath)  # no CMakeLists upstream: fetch only

set(COREMATH_FUNCTIONS
  binary32/acos/acosf binary32/asin/asinf binary32/atan/atanf
  binary32/atan2/atan2f binary32/cos/cosf binary32/log/logf
  binary32/log2/log2f binary32/log10/log10f binary32/pow/powf
  binary32/sin/sinf binary32/tan/tanf
  binary64/atan/atan binary64/cos/cos binary64/exp/exp binary64/log/log
  binary64/pow/pow binary64/sin/sin)
set(_coremath_sources "")
foreach(f IN LISTS COREMATH_FUNCTIONS)
  list(APPEND _coremath_sources "${coremath_SOURCE_DIR}/src/${f}.c")
endforeach()
add_library(coremath STATIC ${_coremath_sources})
set_target_properties(coremath PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
# Exactly as written: no contraction into FMA (the code calls fma itself).
target_compile_options(coremath PRIVATE -O2 -ffp-contract=off -fno-fast-math)
