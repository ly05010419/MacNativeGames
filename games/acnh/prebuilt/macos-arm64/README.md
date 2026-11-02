# The shader translator, ready-made

`totk_xlate` is the program the game starts to translate its shaders (`totk_xlate --service`): the Ryujinx shader
translator (`vendor/ryujinx`, MIT) with this project's patches (`games/totk/tools/totk_xlate/patches`) and the service
(`games/totk/tools/totk_xlate/Service.cs`: Maxwell code -> SPIR-V -> SPIRV-Cross -> Metal Shading Language and its
reflection file), compiled ahead of time with .NET NativeAOT for arm64 macOS. SPIRV-Cross is compiled into it, so no
separate `spirv-cross` program is needed. It is here so that building the app needs no .NET, no network and nothing
outside this folder, and the app needs no .NET runtime and no Python.

- It is the same file as `games/wonder/prebuilt/macos-arm64/totk_xlate`, byte for byte (the build ACNH was brought up
  with; `games/kirby` carries an earlier build):
  SHA-256 `b3c7dcef2331fcdd94657d3a191ec66ac35159f1ac9dc88f7d7e8c6710d7dc08`. It was built with
  `games/totk/tools/totk_xlate/publish_native.sh` from the sources in this repository (.NET SDK 10.0.301); see
  `games/wonder/prebuilt/macos-arm64/README.md` for how that build was checked.
- `totk_xlate.sha256` is its SHA-256. `build_host.py` copies it to `<work>/build/shader-service/totk_xlate` for the
  diagnostic host, and `scripts/package_native_app.py` copies it into the app's `build/shader-service/totk_xlate`
  (where `third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m` looks for it). Both refuse a file with a different hash.
- NativeAOT builds are not byte-for-byte reproducible across folders, so a rebuild gets a different hash; replace both
  files together, and check the rebuild translates the same as described in Wonder's README.
