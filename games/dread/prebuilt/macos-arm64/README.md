Metroid Dread's copy of the shader translator. It is byte-identical to the verified binaries already in the repository:
`totk_xlate` has SHA-256 `6dd53d6486c96e3a95ae5fd49fdc35f75f6e603ff8ededa3d7250fd2e4fe1220` (`totk_xlate.sha256`), the
same file as `games/alc/prebuilt/macos-arm64/totk_xlate` (17eafc1a), `games/by3/prebuilt/macos-arm64/totk_xlate`
(947d4c09) and `games/eow/prebuilt/macos-arm64/totk_xlate` (d139cb41). Those are Bayonetta 3's copy of
`games/totk/prebuilt/macos-arm64`, taken when PR #62 was merged, and the copy is not updated with the Tears of the
Kingdom one. `build.py` and `make shader-tools` refuse a file with another hash. Its notes, as they were:

# The shader translator, ready-made

`totk_xlate` is the program the game starts to translate its shaders (`totk_xlate --service`): the Ryujinx shader
translator (`vendor/ryujinx`, MIT) with this project's patches (`games/totk/tools/totk_xlate/patches`) and the service
(`games/totk/tools/totk_xlate/Service.cs`: Maxwell code -> SPIR-V -> SPIRV-Cross -> Metal Shading Language and its
reflection file), compiled ahead of time with .NET NativeAOT for arm64 macOS. It is here so that building the app needs
no .NET and the app needs no .NET runtime and no Python.

- Built with `games/totk/tools/totk_xlate/publish_native.sh` from the sources in this repository (.NET SDK 10.0.401).
- `totk_xlate.sha256` is its SHA-256; `build_app.py` refuses to package a different file.
- NativeAOT builds are not byte-for-byte reproducible across folders (paths end up in the binary), so a rebuild gets a
  different hash; what matters is that it translates the same. Check a rebuild by replaying a shader cache through it
  and comparing every MSL, SPIR-V and reflection file with the previous build (the development tree has
  `scripts/diag/retranslate_cache.py --service <program> --compare <cache>` for that). The current build gives the same
  output as the .NET translator with the Python service for all 7,570 shader variants of the development cache.
