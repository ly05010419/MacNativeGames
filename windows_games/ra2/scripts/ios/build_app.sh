#!/usr/bin/env bash
# Build the iOS app (unsigned) into build/ios/ra2.app from the lifted code
# in game/generated (scripts/build.sh, on this Mac, from your own files);
# the libraries it needs for iOS are built first when they are not there
# yet (scripts/ios/build_deps.sh).
#
#   scripts/ios/build_app.sh
#   python3 -I scripts/ios/install_device.py --game game/install
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${IOS_BUILD_DIR:-$ROOT/build}"   # IOS_BUILD_DIR: where the iOS builds go (default build/)
PREFIX="$OUT/ios-deps/prefix"
IOS_MIN="${IOS_MIN:-17.0}"
# DEVELOPER_DIR: an Xcode whose path has no spaces (a symlink to it will
# do: configure scripts break on spaces, and xcrun resolves symlinks).
if [ -z "${DEVELOPER_DIR:-}" ] && [ -d "$OUT/Xcode.app" ]; then
  DEVELOPER_DIR="$OUT/Xcode.app/Contents/Developer"
fi
DEVELOPER_DIR="${DEVELOPER_DIR:-$(xcode-select -p)}"
export DEVELOPER_DIR
SDK="$DEVELOPER_DIR/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk"
TOOLCHAIN="$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr/bin"
[ -d "$SDK" ] && [ -x "$TOOLCHAIN/clang" ] || {
  echo "ERROR: no iOS SDK in $DEVELOPER_DIR: install Xcode with the iOS platform" >&2
  exit 1
}
GENERATED_DIR="${GENERATED_DIR:-$ROOT/game/generated}"
[ -f "$GENERATED_DIR/ra2/table.cpp" ] || {
  echo "ERROR: no lifted code in $GENERATED_DIR/ra2: run scripts/build.sh first" >&2
  exit 1
}
"$ROOT/scripts/ios/build_deps.sh"   # skips the libraries already built
# pkg-config (ffmpeg) must see only the iOS builds.
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH=""
# -U: pkg-config is asked again on every configure (CMake keeps its answer
# otherwise, and a rebuilt ffmpeg may need another library: Opus needs
# libswresample).
cmake -S "$ROOT" -B "$OUT/ios" -G Ninja -U '__pkg_config_checked_*' -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDK" -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_MIN" \
  -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
  -DCMAKE_C_COMPILER="$TOOLCHAIN/clang" -DCMAKE_CXX_COMPILER="$TOOLCHAIN/clang++" \
  -DRECOMP_STATIC=ON -DRECOMP_HOST_TOOLS=OFF \
  -DRA2_GENERATED="$GENERATED_DIR/ra2" -DYR_GENERATED="$GENERATED_DIR/yr"
cmake --build "$OUT/ios" --target ra2 -j "${JOBS:-6}"

# The app icon: an asset catalog with the one 1024x1024 image, compiled
# into the bundle (Assets.car and the home-screen PNGs).
ICON_DIR="$OUT/ios/icon"
rm -rf "${ICON_DIR:?}"
mkdir -p "$ICON_DIR/Assets.xcassets/AppIcon.appiconset"
cp "$ROOT/src/ios/AppIcon.png" "$ICON_DIR/Assets.xcassets/AppIcon.appiconset/AppIcon.png"
cat > "$ICON_DIR/Assets.xcassets/Contents.json" <<'JSON'
{ "info" : { "author" : "xcode", "version" : 1 } }
JSON
cat > "$ICON_DIR/Assets.xcassets/AppIcon.appiconset/Contents.json" <<'JSON'
{
  "images" : [
    { "filename" : "AppIcon.png", "idiom" : "universal", "platform" : "ios", "size" : "1024x1024" }
  ],
  "info" : { "author" : "xcode", "version" : 1 }
}
JSON
xcrun actool --compile "$OUT/ios/ra2.app" --platform iphoneos \
  --minimum-deployment-target "$IOS_MIN" --target-device iphone --target-device ipad \
  --app-icon AppIcon --output-partial-info-plist "$ICON_DIR/partial.plist" \
  --output-format human-readable-text --notices --warnings --errors \
  "$ICON_DIR/Assets.xcassets" >"$ICON_DIR/actool.log"
echo "built $OUT/ios/ra2.app"
