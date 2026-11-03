# bink_decode: a Bink movie's frames as binkw32 shows them, raw, for
# making compact movies (src/runtime/hle/bink_format.h).
add_executable(bink_decode bink_decode.cpp)
target_include_directories(bink_decode PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(bink_decode PRIVATE PkgConfig::FFMPEG)
target_compile_features(bink_decode PRIVATE cxx_std_20)
