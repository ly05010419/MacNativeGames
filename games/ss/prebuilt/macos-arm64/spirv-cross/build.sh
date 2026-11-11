#!/bin/sh
# Builds dm/gfx/tools/spirv_cross_bin/spirv-cross, the SPIR-V to MSL converter of the shader translation chain (and of the Vulkan and OpenGL layers' shader tools), from a pinned
# upstream commit, so every Mac runs the same build and its fingerprint is the one dmsh_chain.lock records (a Homebrew build is compiled from source on macOS versions without a
# bottle, and its fingerprint never matches even at the same version: GitHub issue #242).
# The build is reproducible: the same commit, options and Xcode give the same bytes in any folder (the embedded --revision timestamp is the commit's time in UTC, SOURCE_DATE_EPOCH).
# A universal binary (arm64 and x86_64), as the translator's launcher is, so an Intel Mac runs it too. SPIRV-Cross is Apache-2.0 (some headers MIT / Khronos): its LICENSE and LICENSES/
# are copied beside the binary.
# After changing the pin: run this, then tool_lock.py --update-lock and dmsh_chain.py --header (a chain change: rebuild the shaders), and check that the MSL is the same as before for the shaders of
# a game (or say why it changes).
# Usage: build.sh [work folder]   (default: a temporary folder; the source is cloned there, the binary and licences are written beside this script)
set -eu
TAG=vulkan-sdk-1.4.363.0                          # the newest SPIRV-Cross release on 2026-10-26, the version Homebrew ships (1.4.363.0)
COMMIT=f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7   # what TAG names; the build stops when it names another
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$(mktemp -d -t spirv-cross)}
mkdir -p "$WORK"
rm -rf "$WORK/src" "$WORK/build"
git clone --quiet --depth 1 --branch "$TAG" https://github.com/KhronosGroup/SPIRV-Cross.git "$WORK/src"
got=$(git -C "$WORK/src" rev-parse HEAD)
[ "$got" = "$COMMIT" ] || { echo "[RED] $TAG is commit $got, not the pinned $COMMIT" >&2; exit 1; }
export SOURCE_DATE_EPOCH="$(git -C "$WORK/src" log -1 --format=%ct)" TZ=UTC
cmake -S "$WORK/src" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SKIP_INSTALL=ON \
    -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_EXE_LINKER_FLAGS='-Wl,-S,-x' > "$WORK/configure.log"
cmake --build "$WORK/build" --target spirv-cross -j "$(sysctl -n hw.ncpu)" > "$WORK/build.log"
cp "$WORK/build/spirv-cross" "$HERE/spirv-cross"
cp "$WORK/src/LICENSE" "$HERE/LICENSE"
rm -rf "$HERE/LICENSES"; cp -R "$WORK/src/LICENSES" "$HERE/LICENSES"
echo "spirv-cross $TAG ($COMMIT), $(xcodebuild -version | head -1): sha256 $(shasum -a 256 "$HERE/spirv-cross" | cut -c1-64)"
"$HERE/spirv-cross" --revision
