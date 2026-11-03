#!/bin/sh
# make doctor: checks what building and running Pokémon Violet needs, and names the make target (or install) that fixes
# each gap. Plain sh on purpose: it must work when Python is what is missing. The Makefile passes every path.
# Exit code 1 when something the build needs is missing.

missing=0
ok()   { printf '  ok       %s\n' "$1"; }
need() { printf '  MISSING  %s  ->  %s\n' "$1" "$2"; missing=1; }
have() { [ -e "$1" ] && ok "$2" || need "$2" "$3"; }

echo "Tools"
if xcrun --find clang >/dev/null 2>&1; then
  ok "clang: $(xcrun clang --version 2>/dev/null | head -1)"
else
  need "clang" "xcode-select --install"
fi
sdk=$(xcrun --show-sdk-path 2>/dev/null)
sdkver=$(/usr/libexec/PlistBuddy -c 'Print :Version' "$sdk/SDKSettings.plist" 2>/dev/null)
if [ -n "$sdkver" ] && [ "${sdkver%%.*}" -ge "${MACOS_MIN%%.*}" ]; then
  ok "macOS SDK $sdkver"
else
  need "macOS SDK $MACOS_MIN or newer (found: ${sdkver:-none})" "install the current Xcode or Command Line Tools"
fi
pyver=$("$PY" -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null)
if [ "$pyver" = 3.14 ]; then
  ok "Python 3.14: $PY"
  for m in cryptography lz4 capstone unicorn; do
    "$PY" -c "import $m" 2>/dev/null && ok "  $m" || need "  Python package $m" "make venv"
  done
else
  need "Python 3.14 (PY=$PY gives ${pyver:-nothing})" "brew install python@3.14, then make venv"
fi
have "$OPUS/lib/libopus.a" "opus" "brew install opus"
have "$LIBVPX/lib/libvpx.a" "libvpx" "brew install libvpx"
have "$LLVM_BIN/llvm-objdump" "llvm-objdump (make verify)" "brew install llvm@23"
command -v git >/dev/null && ok "git (fetches SPIRV-Cross once)" || need "git" "xcode-select --install"

echo "Work folder $WORK ($VERSION)"
have "$WORK/extracted/$VERSION/exefs/main" "game code" "make extract"
have "$WORK/extracted/$VERSION/romfs" "RomFS" "make extract"
have "$WORK/extracted/firmware/shared-fonts" "shared fonts" "make extract"
have "$WORK/rewritten/$VERSION/main.patched.bin" "rewritten modules" "make rewrite verify"
for g in $GENERATED; do have "$g" "$(basename "$g")" "make prepare"; done
have "$OUT/shader-service/psv_xlate" "shader translator" "make shader-tools"
have "$OUT/spirv-cross/spirv-cross" "SPIRV-Cross" "make shader-tools"
have "$OUT/psv-native-$VERSION" "runtime" "make -j runtime"

[ $missing = 0 ] && echo "Everything is in place: make boot" || echo "Fix the MISSING lines in order, then run make doctor again."
exit $missing
