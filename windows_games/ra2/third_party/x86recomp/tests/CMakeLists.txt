# Dev-only tests; nothing here ships. The EXE-derived oracle tests are
# added by the game project (recomp_add_lift_tests).

# x87_test runs the host CPU's own x87 (inline asm) next to the model from
# the same state and compares the results, so it can only exist on x86-64.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(x87_test x87_test.cpp)
  target_link_libraries(x87_test PRIVATE recomp_x87)
  add_test(NAME x87 COMMAND x87_test)
  set_tests_properties(x87 PROPERTIES TIMEOUT 300)
endif()

# x87_fast_test checks the model's integer fast paths against its own
# SoftFloat paths; portable.
add_executable(x87_fast_test x87_fast_test.cpp)
target_link_libraries(x87_fast_test PRIVATE recomp_x87)
target_compile_options(x87_fast_test PRIVATE -O2 ${RECOMP_X87_FP_FLAGS})
add_test(NAME x87_fast COMMAND x87_fast_test)
set_tests_properties(x87_fast PROPERTIES TIMEOUT 300)

# sse_rsqrt_test: RSQRTPS is the correctly rounded 1/sqrt(x), checked
# exhaustively with exact arithmetic (the host oracle cannot: CPUs differ);
# portable.
add_executable(sse_rsqrt_test sse_rsqrt_test.cpp)
target_link_libraries(sse_rsqrt_test PRIVATE recomp_x87)
target_compile_options(sse_rsqrt_test PRIVATE -O2)
add_test(NAME sse_rsqrt COMMAND sse_rsqrt_test)
set_tests_properties(sse_rsqrt PROPERTIES TIMEOUT 300)

# digital_reverb_test: the Miles-compatible digital reverb against a hash
# of its own output recorded when it matched the original
# (recomp_add_digital_reverb_test compares with the original itself);
# portable.
add_executable(digital_reverb_test digital_reverb_test.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/audio/digital_reverb.cpp)
target_include_directories(digital_reverb_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(digital_reverb_test PRIVATE cxx_std_20)
target_compile_options(digital_reverb_test PRIVATE -O2 -ffp-contract=off)
add_test(NAME digital_reverb COMMAND digital_reverb_test)

# fast2d_test: Miles 6.5's sample levels and Fast 2D against a hash of
# their own output recorded when they matched the original
# (recomp_add_fast2d_test compares with the original itself); portable.
add_executable(fast2d_test fast2d_test.cpp ${PROJECT_SOURCE_DIR}/src/runtime/audio/miles65.cpp)
target_include_directories(fast2d_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(fast2d_test PRIVATE cxx_std_20)
target_compile_options(fast2d_test PRIVATE -O2 -ffp-contract=off)
add_test(NAME fast2d COMMAND fast2d_test)

# gl_draw_test: the GL context's vertex arrays and immediate mode against
# a reference model of GL's conversions, through a recording device;
# portable.
add_executable(gl_draw_test gl_draw_test.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/gl/context.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/gfx/format.cpp)
target_include_directories(gl_draw_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(gl_draw_test PRIVATE cxx_std_20)
target_compile_options(gl_draw_test PRIVATE -O2)
add_test(NAME gl_draw COMMAND gl_draw_test)

# pacing_test: FramePacer's rate and that it waits without spinning;
# portable. Timing-sensitive, so it runs alone.
add_executable(pacing_test pacing_test.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/pacing.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/platform.cpp)
target_include_directories(pacing_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(pacing_test PRIVATE cxx_std_20)
add_test(NAME pacing COMMAND pacing_test)
set_tests_properties(pacing PROPERTIES RUN_SERIAL TRUE)

# win32_text_test: UTF-8 <-> UTF-16 conversion (runtime/hle/win32.h);
# portable.
add_executable(win32_text_test win32_text_test.cpp)
target_include_directories(win32_text_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(win32_text_test PRIVATE cxx_std_20)
add_test(NAME win32_text COMMAND win32_text_test)

# codepage_test: ANSI code pages (runtime/hle/codepage.h): Windows-1252
# and Big5 through the host's iconv.
add_executable(codepage_test codepage_test.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/codepage.cpp)
target_include_directories(codepage_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(codepage_test PRIVATE cxx_std_20)
target_link_libraries(codepage_test PRIVATE recomp_image Iconv::Iconv SDL3::SDL3)
add_test(NAME codepage COMMAND codepage_test)

# cfb_test: compound files (runtime/cfb.h), against [MS-CFB]; portable.
add_executable(cfb_test cfb_test.cpp ${PROJECT_SOURCE_DIR}/src/runtime/cfb.cpp)
target_include_directories(cfb_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(cfb_test PRIVATE cxx_std_20)
add_test(NAME cfb COMMAND cfb_test)

# bink_format_test: compact movies (runtime/hle/bink_format.h); needs
# ffmpeg's headers.
if(FFMPEG_FOUND)
  add_executable(bink_format_test bink_format_test.cpp)
  target_include_directories(bink_format_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(bink_format_test PRIVATE PkgConfig::FFMPEG)
  target_compile_features(bink_format_test PRIVATE cxx_std_20)
  add_test(NAME bink_format COMMAND bink_format_test)
endif()

# touch_gestures_test: touch-screen gestures as mouse actions
# (runtime/hle/touch_gestures.h); portable.
add_executable(touch_gestures_test touch_gestures_test.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/touch_gestures.cpp)
target_include_directories(touch_gestures_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(touch_gestures_test PRIVATE cxx_std_20)
add_test(NAME touch_gestures COMMAND touch_gestures_test)

# frame_layout_test: frames laid out in the output, with an enlarged side
# panel (runtime/gfx/device.h); portable.
add_executable(frame_layout_test frame_layout_test.cpp)
target_link_libraries(frame_layout_test PRIVATE recomp_gfx)
add_test(NAME frame_layout COMMAND frame_layout_test)

# lazy_mem_test: lazily read-back images (DirectDraw Lock of GPU
# surfaces) through real page faults; portable.
add_executable(lazy_mem_test lazy_mem_test.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/lazy_mem.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/guest_memory.cpp
               ${PROJECT_SOURCE_DIR}/src/runtime/guest_alloc.cpp)
target_include_directories(lazy_mem_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(lazy_mem_test PRIVATE cxx_std_20)
target_link_libraries(lazy_mem_test PRIVATE Threads::Threads)
add_test(NAME lazy_mem COMMAND lazy_mem_test)

# lift_semantics_test checks the integer/flag helpers in lifted.h against
# the host CPU the same way.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(lift_semantics_test lift_semantics_test.cpp)
  target_include_directories(lift_semantics_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_features(lift_semantics_test PRIVATE cxx_std_20)
  target_compile_options(lift_semantics_test PRIVATE -O1)
  add_test(NAME lift_semantics COMMAND lift_semantics_test)
  set_tests_properties(lift_semantics PROPERTIES TIMEOUT 300)
endif()

# HLSL translator: each shader in hlsl/ through recomp_hlslc, then
# glslangValidator (the GLSL must compile for Vulkan), with the reflection
# (constant table, inputs, varyings, samplers) compared with its
# <name>.reflect; error.hlsl must fail.
function(recomp_hlsl_test name src stage)  # ARGN: recomp_hlslc arguments
  string(REPLACE ";" "|" _args "${ARGN}")
  add_test(NAME hlsl_${name} COMMAND "${CMAKE_COMMAND}"
           -DHLSLC=$<TARGET_FILE:recomp_hlslc> -DGLSLANG=${RECOMP_GLSLANG}
           -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/hlsl/${src}.hlsl -DSTAGE=${stage}
           -DARGS=${_args} -DPRELUDE=${CMAKE_CURRENT_SOURCE_DIR}/hlsl/prelude.glsl
           -DOUT=${CMAKE_CURRENT_BINARY_DIR}/hlsl_${name}.${stage}
           -DEXPECT=${CMAKE_CURRENT_SOURCE_DIR}/hlsl/${name}.reflect
           -P ${CMAKE_CURRENT_SOURCE_DIR}/hlsl/run.cmake)
endfunction()
recomp_hlsl_test(vs_features vs_features vert -T vs_3_0 -E main)
recomp_hlsl_test(ps_features ps_features frag -T ps_3_0 -E main)
recomp_hlsl_test(preprocess preprocess frag -T ps_2_0 -E main -D FROM_COMMAND_LINE=0.5)
recomp_hlsl_test(sm2_vs sm2 vert -T vs_2_0 -E vs)
recomp_hlsl_test(sm2_ps sm2 frag -T ps_2_0 -E ps)
add_test(NAME hlsl_error COMMAND recomp_hlslc ${CMAKE_CURRENT_SOURCE_DIR}/hlsl/error.hlsl
         -T ps_3_0 -E main)
set_tests_properties(hlsl_error PROPERTIES WILL_FAIL TRUE)

# The bytecode translator on hand-assembled shaders (shader models
# 1.1-3.0); each translation must compile with glslangValidator.
add_executable(hlsl_bytecode_test hlsl_bytecode_test.cpp)
target_link_libraries(hlsl_bytecode_test PRIVATE recomp_hlsl)
add_test(NAME hlsl_bytecode COMMAND hlsl_bytecode_test ${RECOMP_GLSLANG}
         ${CMAKE_CURRENT_SOURCE_DIR}/hlsl/prelude.glsl ${CMAKE_CURRENT_BINARY_DIR})
