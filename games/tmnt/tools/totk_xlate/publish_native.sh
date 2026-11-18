#!/bin/zsh
# Build the shader translation service the packaged app runs (totk_xlate --service): the Ryujinx shader translator
# (third_party/ryujinx) with this project's patches (patches/*.patch) and the service (Service.cs), compiled ahead of time
# with NativeAOT into one arm64 program that needs no .NET runtime.
# The TMNT private packager accepts this output as an explicit input. Building
# the launcher needs no .NET after this source build. NuGet package restore and
# the NativeAOT compiler may require network access the first time.
# usage: games/tmnt/tools/totk_xlate/publish_native.sh dotnet /private/output-dir
set -e
HERE=${0:A:h}
REPO=${HERE:h:h:h:h}
RYU_SRC=${HERE:h:h}/third_party/ryujinx
DOTNET=${1:-dotnet}
OUT=${2:?pass a private output directory outside the checkout}
mkdir -p "$OUT"
OUT=${OUT:A}
case "$OUT/" in "$REPO/"*) echo 'output must stay outside the checkout' >&2; exit 1;; esac
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM HUP
mkdir -p "$WORK/ryujinx-src" "$WORK/xlate"
test -f "$RYU_SRC/LICENSE.txt" || { echo 'missing TMNT-local Ryujinx source' >&2; exit 1; }
cp -R "$RYU_SRC/." "$WORK/ryujinx-src/"
find "$WORK/ryujinx-src/src" -maxdepth 2 \( -name obj -o -name bin \) -type d -exec rm -rf {} +
for p in "$HERE"/patches/*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done
cp "$HERE"/*.cs "$HERE"/totk_xlate.csproj "$WORK/xlate/"
"$DOTNET" publish "$WORK/xlate/totk_xlate.csproj" -c Release -r osx-arm64 -p:PublishAot=true -p:InvariantGlobalization=true \
  -p:RyuSrc="$WORK/ryujinx-src" -o "$WORK/out"
cp "$WORK/out/totk_xlate" "$OUT/totk_xlate"
shasum -a 256 "$OUT/totk_xlate" | awk '{print $1}' > "$OUT/totk_xlate.sha256"
echo "built $OUT/totk_xlate (sha256 $(cat "$OUT/totk_xlate.sha256"))"
