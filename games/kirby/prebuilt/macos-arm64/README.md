# The shader translator, ready-made

`totk_xlate` is the program the game starts to translate its shaders (`totk_xlate --service`): the Ryujinx shader
translator (`vendor/ryujinx`, MIT) with this project's patches (`games/totk/tools/totk_xlate/patches`) and the service
(`games/totk/tools/totk_xlate/Service.cs`: Maxwell code -> SPIR-V -> SPIRV-Cross -> Metal Shading Language and its
reflection file), compiled ahead of time with .NET NativeAOT for arm64 macOS. It is here so that building the app needs
no .NET and the app needs no .NET runtime and no Python.

- Built with `games/totk/tools/totk_xlate/publish_native.sh` from the sources in this repository (.NET SDK 10.0.301).
- `totk_xlate.sha256` is its SHA-256; `build_app.py` refuses to package a different file.
- NativeAOT builds are not byte-for-byte reproducible across folders (paths end up in the binary), so a rebuild gets a
  different hash; what matters is that it translates the same. Check a rebuild by replaying a shader cache through it
  and comparing every MSL, SPIR-V and reflection file with the previous build (the development tree has
  `scripts/diag/retranslate_cache.py --service <program> --compare <cache>` for that). The build before patch 0005 gave
  the same output as the .NET translator with the Python service for all 7,570 shader variants of the development cache.
  Patch 0005 changes only vertex shaders: over the 18,064 startup variants of a 1.4.3 build, 6,679 vertex functions
  gained the initialisation of their outputs and nothing else changed (see docs/how-it-works.md).
  The depth sampling rewrite after PR #104 (Service.cs MatchSwitchDepthSampling) changes 2,650 of the 20,368 seed MSL
  files of a 1.4.3 build, each exactly by the two rewrites; reflection and every other file are unchanged.
