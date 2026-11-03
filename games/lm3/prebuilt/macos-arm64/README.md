# The shader translator, ready-made

`totk_xlate` is the program the game starts to translate its shaders (`totk_xlate --service`): the Ryujinx shader
translator (`vendor/ryujinx`, MIT) with this project's patches (`games/totk/tools/totk_xlate/patches`) and the service
(`games/totk/tools/totk_xlate/Service.cs`: Maxwell code -> SPIR-V -> SPIRV-Cross -> Metal Shading Language and its
reflection file), compiled ahead of time with .NET NativeAOT for arm64 macOS. It is here so that building the app needs
no .NET and the app needs no .NET runtime and no Python.

- Built with `games/lm3/tools/totk_xlate/publish_native.sh` from the sources in this repository (.NET SDK 10.0.401).
- Rebuilt by the maintainers on 2026-11-03 (after PR #218) with this folder's `publish_native.sh` from the sources in this
  repository and the .NET SDK 10.0.401 (Microsoft's tarball, SHA-512 checked), replacing the contributor's build
  (5ff76e99...), which matched no translator already in the repository. Replayed through both, the 4,356 shader variants
  of a Tears of the Kingdom cache gave byte-identical MSL, SPIR-V and reflection files (17,424 files; spirv-cross
  `aa217aeb`); the same corpus through xc3's translator differs in 12,636 of them, so the comparison sees differences.
  Limit: the corpus is Tears of the Kingdom's shaders, not this game's.
- `totk_xlate.sha256` is its SHA-256; `build_app.py` refuses to package a different file.
- NativeAOT builds are not byte-for-byte reproducible across folders (paths end up in the binary), so a rebuild gets a
  different hash; what matters is that it translates the same. Check a rebuild by replaying a shader cache through it
  and comparing every MSL, SPIR-V and reflection file with the previous build (the development tree has
  `scripts/diag/retranslate_cache.py --service <program> --compare <cache>` for that). The current build gives the same
  output as the .NET translator with the Python service for all 7,570 shader variants of the development cache.
