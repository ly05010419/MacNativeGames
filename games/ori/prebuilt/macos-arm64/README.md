# The shader translator, ready-made

`totk_xlate` is the program the game starts to translate its shaders (`totk_xlate --service`): the Ryujinx shader
translator (`vendor/ryujinx`, MIT) with this project's patches (`games/ori/tools/totk_xlate/patches`) and the service
(`games/ori/tools/totk_xlate/Service.cs`: Maxwell code -> SPIR-V -> SPIRV-Cross -> Metal Shading Language and its
reflection file), compiled ahead of time with .NET NativeAOT for arm64 macOS. It is here so that building the app needs
no .NET and the app needs no .NET runtime and no Python.

- Built with `games/ori/tools/totk_xlate/publish_native.sh` from the sources in this repository (.NET SDK 10.0.402 by the
  port's author).
- Rebuilt by the maintainers on 2026-10-28 (after PR #1226) with this folder's `publish_native.sh` from the sources in this
  repository and the .NET SDK 10.0.401 (Microsoft's tarball, SHA-512 checked), replacing the contributor's build (c111c369...).
  Replayed through both, the 4,356 + 1,204 shader variants of two Tears of the Kingdom caches gave byte-identical MSL,
  SPIR-V and reflection files (22,240 files; spirv-cross `aa217aeb` where the service runs it as a program).
  Service.cs passes a compute shader's shared memory size ("shared"): Ori's bloom kernels need it (without it, bands
  every 75 rows through bright areas).
- `totk_xlate.sha256` is its SHA-256; `make runtime` refuses a different file.
- NativeAOT builds are not byte-for-byte reproducible across folders (paths end up in the binary), so a rebuild gets a
  different hash; what matters is that it translates the same. Check a rebuild by replaying a shader cache through it
  and comparing every MSL, SPIR-V and reflection file with the previous build (the development tree has
  `scripts/diag/retranslate_cache.py --service <program> --compare <cache>` for that). The current build gives the same
  output as the .NET translator with the Python service for all 7,570 shader variants of the development cache.
