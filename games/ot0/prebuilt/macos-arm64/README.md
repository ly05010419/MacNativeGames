# The shader translator, ready-made (Octopath Traveler 0)

`totk_xlate` is the program the game starts to translate its shaders (`totk_xlate --service`): the Ryujinx shader
translator (`vendor/ryujinx`, MIT) with this game's patches (`games/ot0/tools/totk_xlate/patches`, 0001–0007; 0006
and 0007 are not in Tears of the Kingdom's copy) and its service (`games/ot0/tools/totk_xlate/Service.cs`: Maxwell
code -> SPIR-V -> SPIRV-Cross -> Metal Shading Language and its reflection file), compiled ahead of time with .NET
NativeAOT for arm64 macOS. It is here so that building the app needs no .NET and the app needs no .NET runtime.

- Built by the port's author with `games/ot0/tools/totk_xlate/publish_native.sh` from the sources in that folder
  (PR #1149). Its SHA-256 (`totk_xlate.sha256`, 311ef273...) matches no other game's copy in this repository; the
  maintainers have not rebuilt it themselves (no .NET SDK on the machine that merged it).
- The build (`games/ot0/Makefile`, `shader-tools`) refuses a file with a different hash.
- NativeAOT builds are not byte-for-byte reproducible across folders (paths end up in the binary), so a rebuild gets a
  different hash; what matters is that it translates the same. Check a rebuild by replaying a shader cache through
  both programs and comparing every MSL, SPIR-V and reflection file.
