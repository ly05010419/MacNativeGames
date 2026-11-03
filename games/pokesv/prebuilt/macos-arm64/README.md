# The shader translator, ready-made

`psv_xlate` is byte-identical to `games/xc3/prebuilt/macos-arm64/totk_xlate` (same SHA-256): copied with the port, not rebuilt.
Its source, `games/pokesv/tools/psv_xlate`, is kept byte-identical to `games/xc3/tools/totk_xlate` (only `publish_native.sh`
differs, writing here), so the binary is the one made from the sources here; that is why the translator keeps the earlier
port's names (`totk_xlate.csproj`, `TOTK_SHADER_DEBUGMODE`, which `psv_shader_rt.m` sets from `PSV_SHADER_DEBUGMODE`).
The notes below are that build's history (paths now under games/pokesv).

`psv_xlate` is the program the game starts to translate its shaders (`psv_xlate --service`): the Ryujinx shader
translator (`vendor/ryujinx`, MIT) with this game's patches (`games/pokesv/tools/psv_xlate/patches`, 0001–0009; 0008
and 0009, transform feedback and geometry shaders as compute, are not in Octopath Traveler 0's copy) and its service
(`games/pokesv/tools/psv_xlate/Service.cs`), compiled ahead of time with .NET NativeAOT for arm64 macOS. It is here so
that building the app needs no .NET and the app needs no .NET runtime.

- Built first by the port's author with `games/pokesv/tools/psv_xlate/publish_native.sh` (.NET SDK 10.0.403, PR #1309,
  8a2be76e...).
- Rebuilt by the maintainers on 2026-10-29 (after PR #1309) with this folder's `publish_native.sh` from the sources in this
  repository and the .NET SDK 10.0.401 (Microsoft's tarball, SHA-512 checked), replacing the contributor's build.
  Replayed through both, the 4,356 shader variants of an earlier port's cache gave byte-identical MSL, SPIR-V and
  reflection files (17,424 files; Ori's translator, run the same way, differs in 12,636 of them, so the comparison sees
  differences). Limit: those shaders use neither transform feedback nor geometry shaders, so patches 0008 and 0009 were
  not exercised by the comparison; this build is the one made from the sources here.
- NativeAOT builds are not byte-for-byte reproducible across folders (paths end up in the binary), so a rebuild gets a
  different hash; what matters is that it translates the same. Check a rebuild by replaying a shader cache through
  both programs and comparing every MSL, SPIR-V and reflection file.
