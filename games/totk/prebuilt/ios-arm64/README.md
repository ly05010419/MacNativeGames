# The shader translator for the iPhone / iPad, ready-made

`libtotk_xlate.a` is the Mac's shader translation service (`../macos-arm64/totk_xlate`, see its README) as a static
library for the iPhone and iPad app: the Ryujinx shader translator (`vendor/ryujinx`, MIT) with this project's patches
(`games/totk/tools/totk_xlate/patches`) and the service (`Service.cs`), compiled ahead of time with .NET NativeAOT for
ios-arm64 (`NativeLib=Static`), merged with the parts of the .NET NativeAOT runtime it needs (MIT). The app cannot start
a process, so it calls the service through `LibExports.cs` (`totk_xlate_init`, `totk_xlate_request`) instead: a shader
variant the app's startup shaders lack is translated on the device, in the background, by the same translator the Mac
uses, and its draws are skipped only until it is ready (totk_shader_rt.m `DEVICE_XLATE`). Before, they were skipped
until the Mac filled the variant in (`ios/tools/fill_shaders.py`).

- Built with `games/totk/tools/totk_xlate/publish_ios_lib.sh` from the sources in this repository (.NET SDK 10.0.403,
  NativeAOT runtime pack 10.0.14). Not JIT: everything is native code made here and signed with the app.
- SPIRV-Cross is not in it: `build_app.py` (`build_device_translator`) compiles it for the device from the pinned
  sources when it makes an iPhone / iPad app, and `ios/tools/build_ipa.py` links both. visionOS apps do without.
- `libtotk_xlate_bootstrap.o` is the runtime pack's `libbootstrapperdll.o`, unchanged: its static initializer starts
  the .NET runtime, and no symbol refers to it, so inside the archive the linker would leave it out (2026-11-03: the
  first call on an iPhone then stopped in `ThreadStore::AttachCurrentThread`); `build_ipa.py` links it as a file.
- `libtotk_xlate.a.sha256` and `libtotk_xlate_bootstrap.o.sha256` are their SHA-256; `build_app.py` refuses to
  package different files.
- NativeAOT builds are not byte-for-byte reproducible, so what is checked is that it translates the same. The library
  form, built from these sources, translated the 551 variants of a Breath of the Wild 1.9.0 runtime cache to the same
  2,204 files (MSL, SPIR-V, reflection) as the shipped `totk_xlate`, byte for byte, both linked into a Mac program
  (osx-arm64) and run in the iOS simulator (iossimulator-arm64) (2026-11-03).
- Rebuilt with Switch 2 / Ampere and geometry support (2026-11-04). The same sources as a Mac static library replayed
  796 real requests (551 Breath of the Wild, 245 Bananza including all 205 compute programs): 796 succeed, all 3,184
  MSL, SPIR-V, reflection and resource files byte-identical to the current Mac translator. Device-library execution
  is verified separately on a physical device; this replay checks the shared translator sources.
