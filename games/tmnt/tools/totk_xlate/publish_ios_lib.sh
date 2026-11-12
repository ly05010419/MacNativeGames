#!/bin/zsh
# Build TMNT's in-process shader translator for a signed iPad application.
# Outputs are private build artifacts outside the repository, never game data.
# Usage: publish_ios_lib.sh /path/to/dotnet /private/output-dir
set -e
HERE=${0:A:h}
REPO=${HERE:h:h:h:h}
RYU_SRC=${HERE:h:h}/third_party/ryujinx
DOTNET=${1:?pass the .NET 10 SDK executable}
OUT=${2:?pass a private output directory outside the checkout}
mkdir -p "$OUT"
OUT=${OUT:A}
case "$OUT/" in "$REPO/"*) echo 'output must stay outside the checkout' >&2; exit 1;; esac
test -f "$RYU_SRC/LICENSE.txt" || { echo 'missing TMNT-local Ryujinx source' >&2; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM HUP
mkdir -p "$WORK/ryujinx-src" "$WORK/xlate"
cp -R "$RYU_SRC/." "$WORK/ryujinx-src/"
find "$WORK/ryujinx-src/src" -maxdepth 2 \( -name obj -o -name bin \) -type d -exec rm -rf {} +
for p in "$HERE"/patches/*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done
cp "$HERE"/*.cs "$HERE"/totk_xlate.csproj "$WORK/xlate/"
"$DOTNET" publish "$WORK/xlate/totk_xlate.csproj" -c Release -r ios-arm64 \
  -p:PublishAot=true -p:PublishAotUsingRuntimePack=true -p:TmntXlateLib=true \
  -p:InvariantGlobalization=true -p:RyuSrc="$WORK/ryujinx-src" -o "$WORK/out"
# The NativeAOT bootstrapper must be linked as a separate object. An archive
# member without a referenced symbol would be omitted before initialization.
NUGET=${NUGET_PACKAGES:-${HOME}/.nuget/packages}
PACKS=("$NUGET"/microsoft.netcore.app.runtime.nativeaot.ios-arm64/*/runtimes/ios-arm64/native)
(( ${#PACKS} > 0 )) || { echo 'iOS NativeAOT runtime pack missing' >&2; exit 1; }
PACK=${PACKS[-1]}
RUNTIME=(libRuntime.WorkstationGC.a libeventpipe-disabled.a libstandalonegc-disabled.a libaotminipal.a
         libSystem.Native.a libSystem.Globalization.Native.a libSystem.IO.Compression.Native.a
         libSystem.Security.Cryptography.Native.Apple.a libbrotlienc.a libbrotlidec.a libbrotlicommon.a libstdc++compat.a)
for member in "${RUNTIME[@]}" libbootstrapperdll.o; do
  test -f "$PACK/$member" || { echo "NativeAOT runtime component missing: $member" >&2; exit 1; }
done
/usr/bin/libtool -static -no_warning_for_no_symbols -o "$OUT/libtmnt_xlate.a" \
  "$WORK/out/totk_xlate.a" "${RUNTIME[@]/#/$PACK/}"
cp "$PACK/libbootstrapperdll.o" "$OUT/libtmnt_xlate_bootstrap.o"
shasum -a 256 "$OUT/libtmnt_xlate.a" "$OUT/libtmnt_xlate_bootstrap.o"
