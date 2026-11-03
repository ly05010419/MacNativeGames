cmake_minimum_required(VERSION 3.25)
project(WinNativeGames CXX C)

# x86recomp (upstream 1c90fe3 with our changes, third_party/x86recomp/UPSTREAM.md).
set(X86RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/x86recomp" CACHE PATH "")
set(CMAKE_LINK_DEPENDS_NO_SHARED ON)  # no relink when only a runtime library changes
enable_testing()
add_subdirectory("${X86RECOMP_DIR}" x86recomp)

# The player's own install, the games' default directory.
set(GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/game/install" CACHE PATH "")

# The recompiled modules, generated from the player's own files (git-ignored).
# Without them only x86recomp is built, its lifter among it: scripts/build.sh
# builds the lifter, lifts and configures again.
set(RA2_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/game/generated/ra2" CACHE PATH "")
recomp_add_lifted(ra2_lifted "${RA2_GENERATED}")
if(NOT TARGET ra2_lifted)
  message(STATUS "no lifted code in ${RA2_GENERATED}: building x86recomp only (scripts/build.sh lifts and builds the games)")
  return()
endif()

# What both games' executables share: Red Alert 2's code at either game's
# addresses (src/ra2/addresses.h).
set(PLAY_SOURCES src/ra2/addresses.cpp src/ra2/controls.cpp src/ra2/game_dir.cpp src/ra2/handshake.cpp
  src/ra2/game_settings.cpp src/ra2/resolution.cpp src/ra2/devtools.cpp src/ra2/scroll.cpp
  src/ra2/sidebar.cpp src/ra2/frame_pacing.cpp src/ra2/commandbar.cpp)
add_executable(ra2 src/ra2/main.cpp src/ra2/launcher.cpp src/ra2/debuglog.cpp src/ra2/hooks.cpp
  ${PLAY_SOURCES} $<TARGET_OBJECTS:ra2_lifted>)
target_compile_features(ra2 PRIVATE cxx_std_20)
target_include_directories(ra2 PRIVATE "${X86RECOMP_DIR}/src")
target_compile_definitions(ra2 PRIVATE
  RA2_DEFAULT_GAME_DIR="${GAME_DIR}")
# recomp_d3d7 carries ddraw.dll; dsound.dll and binkw32.dll are imported by GAME.EXE.
recomp_link_hle(ra2 recomp_runtime recomp_d3d7 recomp_dsound recomp_bink)

# Yuri's Revenge (gamemd.exe; scripts/lift.sh with LIFT_TARGET=yr), when
# its lifted code is there: on the Mac its own executable, yr; on iOS in
# the same app as Red Alert 2, which asks which game to play
# (src/app/chooser.cpp). Both lifts name their functions
# recomp::gen::f_<VA> and their table recomp::kLiftedFunctions, so Yuri's
# Revenge's are renamed (gen_yr, kLiftedFunctionsYr) where they are
# compiled, and each game registers its own table when it starts.
set(YR_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/game/generated/yr" CACHE PATH "")
if(EXISTS "${YR_GENERATED}/table.cpp")
  recomp_add_lifted(yr_lifted "${YR_GENERATED}")
  target_compile_definitions(yr_lifted PRIVATE gen=gen_yr kLiftedFunctions=kLiftedFunctionsYr
                             kLiftedFunctionCount=kLiftedFunctionCountYr)
  # The replacements of its functions are in its renamed namespace too.
  set_source_files_properties(src/yr/hooks.cpp PROPERTIES COMPILE_DEFINITIONS "gen=gen_yr")
  set(YR_SOURCES src/yr/main.cpp src/yr/settings.cpp src/yr/hooks.cpp)
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(ra2 PRIVATE ${YR_SOURCES} src/app/chooser.cpp $<TARGET_OBJECTS:yr_lifted>)
    target_compile_definitions(ra2 PRIVATE WNG_CHOOSER=1)
  else()
    add_executable(yr ${YR_SOURCES} ${PLAY_SOURCES} $<TARGET_OBJECTS:yr_lifted>)
    target_compile_features(yr PRIVATE cxx_std_20)
    target_include_directories(yr PRIVATE "${X86RECOMP_DIR}/src")
    target_compile_definitions(yr PRIVATE
      YR_DEFAULT_GAME_DIR="${GAME_DIR}")
    recomp_link_hle(yr recomp_runtime recomp_d3d7 recomp_dsound recomp_bink)
  endif()
endif()

# iOS: an app bundle (scripts/ios/build_app.sh configures this build; the
# game files are the player's, copied into its Documents, never bundled).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(RA2_BUNDLE_ID "org.winnativegames.ra2" CACHE STRING "iOS bundle identifier")
  set_target_properties(ra2 PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/ios/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${RA2_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "Red Alert 2"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
    MACOSX_BUNDLE_BUNDLE_VERSION "1")
endif()
