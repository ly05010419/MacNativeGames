# The shader translator, ready-made (Xenoblade Chronicles 3)

`totk_xlate` is the program the game starts to translate its shaders (`totk_xlate --service`): the Ryujinx shader
translator (`vendor/ryujinx`, MIT) with this game's patches (`games/xc3/tools/totk_xlate/patches`, 0001–0009; 0008
and 0009, transform feedback and geometry shaders as compute, are not in Octopath Traveler 0's copy) and its service
(`games/xc3/tools/totk_xlate/Service.cs`), compiled ahead of time with .NET NativeAOT for arm64 macOS. It is here so
that building the app needs no .NET and the app needs no .NET runtime.

- Built first by the port's author with `games/xc3/tools/totk_xlate/publish_native.sh` (.NET SDK 10.0.403, PR #1309,
  8a2be76e...).
- Rebuilt by the maintainers on 2026-10-29 (after PR #1309) with this folder's `publish_native.sh` from the sources in this
  repository and the .NET SDK 10.0.401 (Microsoft's tarball, SHA-512 checked), replacing the contributor's build.
  Replayed through both, the 4,356 shader variants of a Tears of the Kingdom cache gave byte-identical MSL, SPIR-V and
  reflection files (17,424 files; Ori's translator, run the same way, differs in 12,636 of them, so the comparison sees
  differences). Limit: those shaders use neither transform feedback nor geometry shaders, so patches 0008 and 0009 were
  not exercised by the comparison; this build is the one made from the sources here.
- Replaced by PR #1917's own build (2026-11-09: patch 0010, one-lane subgroup operations in vertex shaders). It was checked
  the same way: a rebuild from this repository's sources with the .NET SDK 10.0.401 and this build, replaying the 4,356
  Tears of the Kingdom variants, gave 17,420 byte-identical files (Ori's translator differs in 12,635). Limit: those
  shaders do not use subgroup operations in vertex shaders, so patch 0010 itself was not exercised. The rebuild was not
  committed (git on the maintainer's Mac was killed whenever it hashed that file), so the binary here is the contributor's.
- NativeAOT builds are not byte-for-byte reproducible across folders (paths end up in the binary), so a rebuild gets a
  different hash; what matters is that it translates the same. Check a rebuild by replaying a shader cache through
  both programs and comparing every MSL, SPIR-V and reflection file.
