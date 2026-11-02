#!/bin/zsh
# Build the shader translation service the packaged app runs (totk_xlate --service): the Ryujinx shader translator
# (vendor/ryujinx) with this project's patches (patches/*.patch) and the service (Service.cs), compiled ahead of time
# with NativeAOT into one arm64 program that needs no .NET runtime.
# build_app.py uses the copy in games/ot0/prebuilt/macos-arm64 (made with this script), so building the app needs no
# .NET at all; run this only after changing the translator, its patches or Service.cs. Needs the .NET 10 SDK and network
# access the first time (NuGet packages, the NativeAOT compiler).
# SPIRV-Cross (build_app.py SPIRV_CROSS: the pinned commit, its sources and flags) is compiled in as well
# (spirv_cross_inproc.cpp: its own main.cpp's compile_iteration), so the service needs no process per shader. The
# sources are fetched at that commit; SPIRV_CROSS_SRC=<checkout at that commit> uses a local one instead.
# usage: games/ot0/tools/totk_xlate/publish_native.sh [dotnet]   -> games/ot0/prebuilt/macos-arm64/totk_xlate (+ .sha256)
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
# SPIRV-Cross: the commit, sources and flags build_app.py builds the spirv-cross program from
PIN=$(sed -n "s/^SPIRV_CROSS = ('\([^']*\)', '\([0-9a-f]*\)')$/\1 \2/p" "$REPO/build_app.py")
URL=${PIN% *}; COMMIT=${PIN#* }
[[ ${#COMMIT} == 40 ]] || { echo "no SPIRV_CROSS pin in build_app.py"; exit 1; }
SRC=${SPIRV_CROSS_SRC:-$WORK/spirv-cross-src}
if [[ -z $SPIRV_CROSS_SRC ]]; then
  git init -q "$SRC" && git -C "$SRC" fetch -q --depth 1 "$URL" "$COMMIT" && git -C "$SRC" checkout -q FETCH_HEAD
fi
[[ $(git -C "$SRC" rev-parse HEAD) == $COMMIT ]] || { echo "$SRC is not SPIRV-Cross $COMMIT"; exit 1; }
SOURCES=(${(f)"$(uv run --no-project python -c "import ast,re,sys; t=open(sys.argv[1]).read(); print(chr(10).join(s for s in ast.literal_eval(re.search(r'^SPIRV_CROSS_SOURCES = (\(.*?\))$', t, re.M|re.S).group(1)) if s != 'main.cpp'))" "$REPO/build_app.py")"})
FLAGS=(${(f)"$(uv run --no-project python -c "import ast,re,sys; t=open(sys.argv[1]).read(); print(chr(10).join(ast.literal_eval(re.search(r'^SPIRV_CROSS_FLAGS = (\(.*?\))$', t, re.M).group(1))))" "$REPO/build_app.py")"})
FLAGS+=(-mmacosx-version-min=12.0)   # as the NativeAOT link of the service (otherwise every object warns and may need a newer macOS)
mkdir -p "$WORK/spvc"
for f in "${SOURCES[@]}"; do xcrun clang++ "${FLAGS[@]}" -c "$SRC/$f" -o "$WORK/spvc/${f%.cpp}.o" & done
xcrun clang++ "${FLAGS[@]}" -I"$SRC" -c "$HERE/spirv_cross_inproc.cpp" -o "$WORK/spvc/spirv_cross_inproc.o" &   # includes main.cpp
wait
xcrun ar rcs "$WORK/libtotk_spirv_cross.a" "$WORK"/spvc/*.o
"$DOTNET" publish "$WORK/xlate/totk_xlate.csproj" -c Release -r osx-arm64 -p:PublishAot=true -p:InvariantGlobalization=true \
  -p:RyuSrc="$WORK/ryujinx-src" -p:TotkSpirvCrossLib="$WORK/libtotk_spirv_cross.a" -o "$WORK/out"
OUT=$REPO/games/ot0/prebuilt/macos-arm64
mkdir -p "$OUT"
cp "$WORK/out/totk_xlate" "$OUT/totk_xlate"
shasum -a 256 "$OUT/totk_xlate" | awk '{print $1}' > "$OUT/totk_xlate.sha256"
echo "built $OUT/totk_xlate (sha256 $(cat "$OUT/totk_xlate.sha256"))"
