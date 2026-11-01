# Native shader translation tool

`totk_xlate` is a build/runtime tool, not a game binary or translated game shader.
It combines the MIT Ryujinx shader translator in `../../third_party/ryujinx`,
this title's `../../tools/totk_xlate/Service.cs`, and the retained patches in
`../../tools/totk_xlate/patches`. NativeAOT produces an arm64 macOS executable
that needs no installed .NET runtime. Sources and licenses are included locally.
See `../../third_party/COPIES.md` for the copied input's provenance.

`build_host.py` checks `totk_xlate.sha256` before staging this tool into the
external host workspace. Rebuild after translator changes with the .NET 10 SDK:

```sh
games/tomodachi/tools/totk_xlate/publish_native.sh /path/to/dotnet
```

Intermediate sources and build products use a temporary directory outside the
checkout. Only the necessary tool prebuilt and its checksum are updated here.
NuGet/network access is needed on a first build. NativeAOT output can vary with
build paths, so compare translation results as well as updating the checksum.
No Tomodachi-wide shader-cache equivalence result is claimed for a rebuild.
