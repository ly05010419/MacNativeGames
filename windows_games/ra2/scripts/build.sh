#!/usr/bin/env bash
# Build the games from your own install in one go: Red Alert 2, and Yuri's
# Revenge when its gamemd.exe is there too.
#
#   scripts/build.sh [install dir]        # default: game/install
#
# In one build folder (BUILD_DIR, default build/main): x86recomp's lifter
# first, then scripts/lift.sh recompiles your executables into
# game/generated/<game> (GENERATED_DIR), then cmake builds build/main/ra2
# (and yr). Running it again rebuilds only what changed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL="$(cd "${1:-$ROOT/game/install}" && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build/main}"
export GENERATED_DIR="${GENERATED_DIR:-$ROOT/game/generated}"
JOBS="${JOBS:-6}"   # 16 GB of memory: 6 compilers at most for the generated code

# -U: pkg-config (ffmpeg) is asked again on every configure, so an upgraded
# library's dependencies are seen (CMake keeps its first answer otherwise).
configure() {
  cmake -S "$ROOT" -B "$BUILD" -G Ninja -U '__pkg_config_checked_*' -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DRECOMP_LLVM_ROOT="${RECOMP_LLVM_ROOT:-/opt/homebrew/opt/llvm}" -DGAME_DIR="$INSTALL" \
    -DRA2_GENERATED="$GENERATED_DIR/ra2" -DYR_GENERATED="$GENERATED_DIR/yr" > /dev/null
}

configure
cmake --build "$BUILD" --target recomp_lift -j "$JOBS"
export LIFTER="$BUILD/x86recomp/tools/lifter/recomp_lift"
targets=(ra2)
"$ROOT/scripts/lift.sh" "$INSTALL"
if ls "$INSTALL" | grep -qix 'gamemd.exe'; then
  LIFT_TARGET=yr "$ROOT/scripts/lift.sh" "$INSTALL"
  targets+=(yr)
fi
configure   # the lifted code is there now
cmake --build "$BUILD" --target "${targets[@]}" -j "$JOBS"
for t in "${targets[@]}"; do echo "built $BUILD/$t"; done
