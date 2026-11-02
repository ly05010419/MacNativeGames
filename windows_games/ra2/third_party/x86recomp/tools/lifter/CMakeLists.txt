# recomp_lift: x86-32 -> C++ lifter (build-time only, never linked into a game).

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis
  GIT_REPOSITORY https://github.com/zyantific/zydis.git
  GIT_TAG a2278f1d254e492f6a6b39f6cb5d1f5d515659dc  # v4.1.1
  GIT_SUBMODULES dependencies/zycore
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(zydis)

add_executable(recomp_lift
  main.cpp
  program.cpp
  function.cpp
  emit.cpp
  x87test.cpp)
target_link_libraries(recomp_lift PRIVATE recomp_image Zydis)
