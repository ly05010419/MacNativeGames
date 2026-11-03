#!/bin/zsh
# Build the shader translation service for the iPhone / iPad app (ios-arm64): the same sources as publish_native.sh
# (vendor/ryujinx with patches/*.patch, Service.cs and the rest), compiled ahead of time with NativeAOT into a static
# library (LibExports.cs: totk_xlate_init / totk_xlate_request), merged with the parts of the .NET NativeAOT runtime it
# needs into one archive. SPIRV-Cross is not in it: build_ipa.py builds it for the device from the pinned sources and links
# both. The app then translates a shader variant its startup shaders lack on the device, in the background, with the
# translator the Mac uses (totk_shader_rt.m), instead of skipping its draws until the Mac fills it in.
# Not JIT: NativeAOT is native code made here; iOS only runs code signed with the app.
# Needs the .NET 10 SDK and network access the first time (NuGet: the NativeAOT runtime pack for ios-arm64).
# The runtime's start-up object (libbootstrapperdll.o, whose static initializer starts the .NET runtime) stays a file of
# its own: no symbol refers to it, so from inside an archive the linker would leave it out and the first call would find
# no runtime (2026-11-03: the iPhone stopped in ThreadStore::AttachCurrentThread); force-loading the archive instead
# fails on the runtime libraries' shared minipal objects. build_ipa.py links the object itself.
# usage: games/totk/tools/totk_xlate/publish_ios_lib.sh [dotnet]
#   -> games/totk/prebuilt/ios-arm64/libtotk_xlate.a and libtotk_xlate_bootstrap.o (+ their .sha256)
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
export NUGET_PACKAGES=${NUGET_PACKAGES:-$WORK/nuget}
# PublishAotUsingRuntimePack: the SDK's own NativeAOT refuses ios-arm64 for a plain net10.0 project (NETSDK1203); the
# runtime pack (Microsoft.NETCore.App.Runtime.NativeAOT.ios-arm64) is what the .NET iOS workload uses.
"$DOTNET" publish "$WORK/xlate/totk_xlate.csproj" -c Release -r ios-arm64 -p:PublishAot=true -p:PublishAotUsingRuntimePack=true \
  -p:TotkXlateLib=true -p:InvariantGlobalization=true -p:RyuSrc="$WORK/ryujinx-src" -o "$WORK/out"
PACK=$(ls -d "$NUGET_PACKAGES"/microsoft.netcore.app.runtime.nativeaot.ios-arm64/*/runtimes/ios-arm64/native | tail -1)
# The runtime parts a NativeLib=Static library needs at the app's link (no ServerGC, event pipe or standalone GC; zlib
# is the system's -lz on iOS).
RUNTIME=(libRuntime.WorkstationGC.a libeventpipe-disabled.a libstandalonegc-disabled.a libaotminipal.a
         libSystem.Native.a libSystem.Globalization.Native.a libSystem.IO.Compression.Native.a
         libSystem.Security.Cryptography.Native.Apple.a libbrotlienc.a libbrotlidec.a libbrotlicommon.a libstdc++compat.a)
OUT=$REPO/games/totk/prebuilt/ios-arm64
mkdir -p "$OUT"
xcrun libtool -static -no_warning_for_no_symbols -o "$OUT/libtotk_xlate.a" "$WORK/out/totk_xlate.a" "${RUNTIME[@]/#/$PACK/}"
cp "$PACK/libbootstrapperdll.o" "$OUT/libtotk_xlate_bootstrap.o"
for f in libtotk_xlate.a libtotk_xlate_bootstrap.o; do shasum -a 256 "$OUT/$f" | awk '{print $1}' > "$OUT/$f.sha256"; done
echo "built $OUT/libtotk_xlate.a ($(du -h "$OUT/libtotk_xlate.a" | cut -f1), sha256 $(cat "$OUT/libtotk_xlate.a.sha256")) with $(basename ${PACK:h:h:h})"
