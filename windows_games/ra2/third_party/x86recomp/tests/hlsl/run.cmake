# One HLSL translator test (tests/CMakeLists.txt): translates SRC with
# recomp_hlslc (ARGS, |-separated) into a complete GLSL source with
# prelude.glsl, compiles it with glslangValidator for Vulkan as STAGE,
# and compares the reflection with EXPECT.
string(REPLACE "|" ";" _args "${ARGS}")
execute_process(COMMAND "${HLSLC}" "${SRC}" ${_args} --prelude "${PRELUDE}" -o "${OUT}"
                RESULT_VARIABLE _r ERROR_VARIABLE _e)
if(_r)
  message(FATAL_ERROR "translation failed: ${_e}")
endif()
execute_process(COMMAND "${GLSLANG}" -V --target-env vulkan1.0 -S ${STAGE} "${OUT}" -o "${OUT}.spv"
                RESULT_VARIABLE _r OUTPUT_VARIABLE _o)
if(_r)
  message(FATAL_ERROR "glslang rejected ${OUT}: ${_o}")
endif()
execute_process(COMMAND "${HLSLC}" "${SRC}" ${_args} --reflect OUTPUT_VARIABLE _reflect RESULT_VARIABLE _r)
file(READ "${EXPECT}" _expect)
if(_r OR NOT _reflect STREQUAL _expect)
  message(FATAL_ERROR "reflection differs from ${EXPECT}:\n${_reflect}")
endif()
