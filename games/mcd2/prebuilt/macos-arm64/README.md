# MCD2 shader translator (macOS arm64)

This is MCD2's independently pinned translator, not the inherited Bayonetta/TOTK binary.
The current candidate was promoted from `MCD2_build/build/shader-service-candidate/totk_xlate`.
SHA-256: `817ec71524f4fb7a081731a3fd7470db629de711c0096c579374b1480107644f`.

Its source and publish script are under `games/mcd2/tools/totk_xlate/`.
The runtime requires pool descriptor counts/index domains and compute shared-memory
reflection. Do not replace this binary with a sibling game's prebuilt without
validating that protocol. `make shader-tools` verifies `totk_xlate.sha256` before
copying it; verify the final app's helper against the same hash after packaging.

Validation for this candidate: 76 production translator descriptor checks passed.
The optional captured-shader specialization test was not run in this validation
because no request fixture was supplied. This is not a full shader-cache replay
or visual acceptance. Legacy cached reflection can still be rejected by the runtime;
updating the translator does not by itself migrate old cache entries.

## Historical upstream notes (not validation of the current binary)

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
