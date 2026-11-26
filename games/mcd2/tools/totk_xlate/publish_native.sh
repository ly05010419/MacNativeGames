#!/bin/zsh
# Rebuild only MCD2's shader service into the flat external worktree.
# Does not replace repository prebuilts, game runtime, or any signed app.
# usage: publish_native.sh <dotnet> <MCD2_build>
set -eu
HERE=${0:A:h}
REPO=${HERE:h:h:h:h}
if (( $# != 2 )); then echo "usage: $0 <dotnet> <MCD2_build>" >&2; exit 2; fi
DOTNET=${1:A}
WORK=${2:A}
SRC=$WORK/src/mcd2-xlate
OUT=$WORK/build/shader-service-candidate
mkdir -p "$SRC/ryujinx-src" "$SRC/xlate" "$OUT"
if [[ ! -f "$SRC/.patched" ]]; then
  cp -R "$REPO/vendor/ryujinx/." "$SRC/ryujinx-src/"
  for p in "$HERE"/patches/*.patch; do patch -s -d "$SRC/ryujinx-src" -p1 --forward < "$p"; done
  touch "$SRC/.patched"
fi
cp "$HERE"/*.cs "$HERE"/totk_xlate.csproj "$SRC/xlate/"
# Keep restore caches and SDK state with the external work, not the internal disk.
export DOTNET_CLI_HOME=$WORK/src/dotnet-home
export NUGET_PACKAGES=$WORK/src/nuget-packages
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
"$DOTNET" publish "$SRC/xlate/totk_xlate.csproj" -c Release -r osx-arm64 \
  -p:PublishAot=true -p:InvariantGlobalization=true -p:RyuSrc="$SRC/ryujinx-src" -o "$OUT"
shasum -a 256 "$OUT/totk_xlate" | awk '{print $1}' > "$OUT/totk_xlate.sha256"
echo "MCD2 candidate built: $OUT/totk_xlate (not installed)"
