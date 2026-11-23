# TMNT's shader translator, ready-made

`totk_xlate` is the program the game starts to translate its shaders (`totk_xlate --service`): the Ryujinx shader
translator in `games/tmnt/third_party/ryujinx` (MIT) with TMNT's patches (`games/tmnt/tools/totk_xlate/patches`) and
service (`games/tmnt/tools/totk_xlate/Service.cs`), compiled ahead of time with .NET NativeAOT for arm64 macOS. It is
TMNT's own build, not a copy of another game's: TMNT has its own Ryujinx copy and patches.

It is here (since 2026-11-22) so that building the app needs no .NET SDK: the MNG Launcher carries no SDK, and players
have none (user 2026-11-21: "去掉 .net sdk"; 2026-11-22: "对 做吧"). Until then every TMNT build compiled it with
`publish_native.sh`. `dotnet-runtime.LICENSE` is the licence of the .NET runtime code linked into it.

- Built with `games/tmnt/tools/totk_xlate/publish_native.sh` from the sources in this repository, .NET SDK 10.0.401.
- `totk_xlate.sha256` is its SHA-256; `scripts/build_shader_tools.py` refuses a different file.
- It is in `tools/translator_changes.json`: a new build must answer why it changed and whether it was necessary.
- NativeAOT builds are not byte-for-byte reproducible across folders, so a rebuild has another hash; check that it
  translates the same by replaying shaders through both and comparing every MSL, SPIR-V and reflection file. This one
  was checked so against a second build from another folder: 179 shaders, all 716 output files identical.
