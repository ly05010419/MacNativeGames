# Third-party code

Code in this repository that comes from other projects keeps its license; the license file sits next to the code.

| Code | From | License | File |
|---|---|---|---|
| `games/totk/third_party/d2r` (NSO rewriter, verification tools) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), adapted | MIT | `games/totk/third_party/d2r/LICENSE` |
| `games/totk/third_party/d2r_gfx` (NVN on Metal, texture tools, host headers) | d2r-mobile, extended for this game | MIT | `games/totk/third_party/d2r_gfx/LICENSE` |
| `games/totk/third_party/smaa` lookup tables and `totk_smaa_shader.h` (Metal adaptation of SMAA 1x presets) | [iryoku/smaa](https://github.com/iryoku/smaa) at `71c806a838bdd7d517df19192a20f0c61b3ca29d`; adapter origin recorded in `games/totk/third_party/smaa/README.md` | MIT | `games/totk/third_party/smaa/LICENSE.txt` |
| `totk_fxaa_shader.h` (Metal adaptation of FXAA 3.11 Quality preset 39) | NVIDIA, Timothy Lottes; reference in [GameTechDev/CMAA2](https://github.com/GameTechDev/CMAA2) at `071c6b0857559f4e36f614362e6d2aab1b61938a`, `Projects/CMAA2/FXAA/Fxaa3_11.h` | NVIDIA BSD-3-Clause-style terms | `games/totk/third_party/fxaa/LICENSE.txt`; copied into Mac apps as `Contents/Resources/totk/licenses/fxaa.txt` |
| texture layout sizes in `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_tex.c` | deko3d | zlib | `games/totk/third_party/d2r_gfx/LICENSE.deko3d` |
| `games/totk/third_party/d2r_gfx/third_party/bcdec` | [bcdec](https://github.com/iOrange/bcdec) 0.985 | MIT or public domain | `games/totk/third_party/d2r_gfx/third_party/bcdec/LICENSE` |
| `games/totk/tools/totk_xlate` (shader translator front end) | d2r-mobile, adapted | MIT | `games/totk/tools/totk_xlate/LICENSE` |
| `vendor/ryujinx` (Ryujinx.Graphics.Shader, Ryujinx.Common, Spv.Generator) | Ryujinx, as pinned by d2r-mobile; patched by `games/totk/tools/totk_xlate/patches` | MIT | `vendor/ryujinx/LICENSE.txt` |
| `games/totk/prebuilt/macos-arm64/totk_xlate` (the shader translator, a native program) | built from `vendor/ryujinx` + the patches + `games/totk/tools/totk_xlate` with `publish_native.sh`; contains .NET runtime libraries compiled in by NativeAOT | MIT (Ryujinx, d2r-mobile, .NET) | `vendor/ryujinx/LICENSE.txt`, `games/totk/tools/totk_xlate/LICENSE` |
| in `games/totk/runtime/threads.c`: SignalToAddress that wakes all waiters subtracts 2, SystemEvent bindings that end with their write handle, signals that wake only the waiters of the signalled object (checked by `games/totk/tests/system_event_bind`) | [nxnative](https://github.com/seedds/nxnative) commits `d47f899`, `49ee623`, `df7badd`, rewritten for this runtime (via PR #159) | MIT | `games/totk/third_party/licenses/nxnative.txt` |
| spin-wait search and copy in `games/totk/third_party/d2r/patch_nso.py` ("Spin waits") and its test `tests/test_spin_waits.py` | [nxnative](https://github.com/seedds/nxnative) commits `066f2af`, `6d34984`, adapted to this rewriter | MIT | `games/totk/third_party/licenses/nxnative.txt` |
| Switch 2 (Ampere / NVN2) shaders: the decoder, emitters and control section in `games/totk/tools/totk_xlate/patches/0006-ampere-switch-2-shaders.patch`; geometry shaders as compute kernels in `patches/0007-fixed-input-geometry-as-compute.patch`, `games/totk/tools/totk_xlate/Geometry.cs` and `GeometryLean.cs` | [nxnative](https://github.com/seedds/nxnative), the commits each file names, adapted | MIT | `games/totk/third_party/licenses/nxnative.txt` |
| geometry primitive assembly `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_geometry.h` and the host kernels in `nvn_geometry_lean.inc` (from its `geo_pack.metal`) | nxnative | MIT | `games/totk/third_party/licenses/nxnative.txt` |
| Switch 2 SDK 20 host services in `games/totk/runtime/ns2_host.c`, `ns2_audio_out.c` and `ns2_codec.c` (behaviour ported) | nxnative, the commits each file names | MIT | `games/totk/third_party/licenses/nxnative.txt` |
| audio effects in `games/totk/runtime/audio_reverb.c`, `games/totk/runtime/audio_effects.c` (delay, reverb, compressor) and the mixer | ported from Ryujinx.Audio | MIT | `games/totk/third_party/licenses/Ryujinx-Audio.txt` |
| `zeldaVR/streamer/basaltvr.patch` (our changes to the streamer, with lines of its source as diff context) | [BasaltVR](https://github.com/frenchbaguetteman/BasaltVR) at `4c46172` | MIT | `zeldaVR/streamer/LICENSE.BasaltVR` |
| `windows_games/ra2/third_party/x86recomp` (x86 to arm64 recompiler and Win32 runtime) | [x86recomp](https://github.com/JTM-rootstorm/x86recomp) at `1c90fe3`, with WinNativeGames' changes (`UPSTREAM.md` lists them) | MIT | `windows_games/ra2/third_party/x86recomp/LICENSE` |
| Scene HDR math and grading in `games/totk/third_party/d2r_gfx/gfx/nvn/totk_hdr_compose.h` | [RenoDX TOTK](https://github.com/Souperman9/renodx/tree/40d764d88719ab06c8e46139b5454e7dafc8cbcc/src/games/totk) | MIT | `games/totk/third_party/d2r_gfx/gfx/nvn/LICENSE.renodx` |

Fetched or linked at build time (not in this repository):

| Code | Use | License |
|---|---|---|
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) at `aa217ae` | SPIR-V to Metal Shading Language | Apache-2.0 |
| [hactool](https://github.com/SciresM/hactool) at `1d64a83` (with explicit private title-key-file hook), including its bundled mbedTLS | Base + update BKTR merge at build time only | ISC (hactool), Apache-2.0 (mbedTLS); see fetched source LICENSE files |
| libopus (Homebrew), linked statically | Opus audio | BSD-3-Clause |
| libvpx (Homebrew), linked statically | VP9 movies | BSD-3-Clause |
| [Berkeley SoftFloat 3](https://github.com/ucb-bar/berkeley-softfloat-3) at `5c06db3`, fetched by `windows_games/ra2/third_party/x86recomp/cmake/softfloat.cmake` | exact x87 arithmetic in windows_games | BSD-3-Clause |
| [CORE-MATH](https://gitlab.inria.fr/core-math/core-math) at `74fcd10`, fetched by `windows_games/ra2/third_party/x86recomp/cmake/coremath.cmake` | correctly rounded math functions in windows_games | MIT |
| SDL3 3.4.14, glslang, SPIRV-Cross, ffmpeg 8.1 (decoders only), built by `windows_games/ra2/scripts/ios/build_deps.sh` | windows_games on iOS | zlib, BSD-3-Clause / Apache-2.0, Apache-2.0, LGPL-2.1 |
| [BasaltVR](https://github.com/frenchbaguetteman/BasaltVR) at `4c46172`, fetched and built by `zeldaVR/install_vr.sh` with `zeldaVR/streamer/basaltvr.patch` | the VR streamer (Mac to Meta Quest 3) | MIT |
| [ALVR](https://github.com/alvr-org/ALVR) client v20.14.0, downloaded (SHA-256 checked) and installed on the headset by `zeldaVR/install_vr.sh` | shows the VR picture on the Meta Quest 3 | MIT |
| [Noto Sans CJK SC](https://github.com/notofonts/noto-cjk) (Source Han Sans) DemiLight at `165c01b`, downloaded and checked by `windows_games/ra2/scripts/languages.py` | the Simplified Chinese glyphs it draws into the player's game font | SIL OFL 1.1 |
| [OpenCC](https://github.com/BYVoid/OpenCC), run (not included) by `windows_games/ra2/scripts/languages.py` | Traditional to Simplified Chinese | Apache-2.0 |
