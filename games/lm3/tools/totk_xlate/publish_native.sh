#!/bin/zsh
# Build the shader translation service the packaged app runs (totk_xlate --service): the Ryujinx shader translator
# (vendor/ryujinx) with this project's patches (patches/*.patch) and the service (Service.cs), compiled ahead of time
# with NativeAOT into one arm64 program that needs no .NET runtime.
# build_app.py uses the copy in games/totk/prebuilt/macos-arm64 (made with this script), so building the app needs no
# .NET at all; run this only after changing the translator, its patches or Service.cs. Needs the .NET 10 SDK and network
# access the first time (NuGet packages, the NativeAOT compiler).
# usage: games/totk/tools/totk_xlate/publish_native.sh [dotnet]   -> games/totk/prebuilt/macos-arm64/totk_xlate (+ .sha256)
set -e
HERE=${0:A:h}
REPO=${HERE:h:h:h:h}
DOTNET=${1:-dotnet}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM HUP
mkdir -p "$WORK/ryujinx-src" "$WORK/xlate"
cp -R "$REPO/vendor/ryujinx/." "$WORK/ryujinx-src/"
find "$WORK/ryujinx-src/src" -maxdepth 2 \( -name obj -o -name bin \) -type d -exec rm -rf {} +
for p in "$HERE"/patches/*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done
cp "$HERE"/*.cs "$HERE"/totk_xlate.csproj "$WORK/xlate/"
"$DOTNET" publish "$WORK/xlate/totk_xlate.csproj" -c Release -r osx-arm64 -p:PublishAot=true -p:InvariantGlobalization=true \
  -p:RyuSrc="$WORK/ryujinx-src" -o "$WORK/out"
OUT=$REPO/games/totk/prebuilt/macos-arm64
mkdir -p "$OUT"
cp "$WORK/out/totk_xlate" "$OUT/totk_xlate"
shasum -a 256 "$OUT/totk_xlate" | awk '{print $1}' > "$OUT/totk_xlate.sha256"
echo "built $OUT/totk_xlate (sha256 $(cat "$OUT/totk_xlate.sha256"))"
