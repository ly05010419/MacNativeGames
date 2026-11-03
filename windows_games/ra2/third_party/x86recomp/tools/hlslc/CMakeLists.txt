# recomp_hlslc: runs the HLSL translator on a file (dev tool: shader dumps,
# tests/hlsl).
add_executable(recomp_hlslc main.cpp)
target_link_libraries(recomp_hlslc PRIVATE recomp_hlsl)
