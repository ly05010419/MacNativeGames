# Third-party code

Code in this repository that comes from other projects keeps its license; the license file sits next to the code.

| Code | From | License | File |
|---|---|---|---|
| `games/totk/third_party/d2r` (NSO rewriter, verification tools) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), adapted | MIT | `games/totk/third_party/d2r/LICENSE` |
| `games/totk/third_party/d2r_gfx` (NVN on Metal, texture tools, host headers) | d2r-mobile, extended for this game | MIT | `games/totk/third_party/d2r_gfx/LICENSE` |
| texture layout sizes in `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_tex.c` | deko3d | zlib | `games/totk/third_party/d2r_gfx/LICENSE.deko3d` |
| `games/totk/third_party/d2r_gfx/third_party/bcdec` | [bcdec](https://github.com/iOrange/bcdec) 0.985 | MIT or public domain | `games/totk/third_party/d2r_gfx/third_party/bcdec/LICENSE` |
| `games/totk/tools/totk_xlate` (shader translator front end) | d2r-mobile, adapted | MIT | `games/totk/tools/totk_xlate/LICENSE` |
| `vendor/ryujinx` (Ryujinx.Graphics.Shader, Ryujinx.Common, Spv.Generator) | Ryujinx, as pinned by d2r-mobile; patched by `games/totk/tools/totk_xlate/patches` | MIT | `vendor/ryujinx/LICENSE.txt` |
| `games/totk/tools/totk_xlate/patches/0006-runtime-texel-offsets-are-not-constants.patch` (texel offsets read from a register) | [nxnative](https://github.com/seedds/nxnative) commit `b3f25a0`, adapted | MIT | `games/totk/third_party/licenses/nxnative.txt` |
| `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_cap_auto.h` (auto frame-rate cap probes: remembered cost, efficiency-core and GPU-clock signals, hold-off and grace) | [nxnative](https://github.com/seedds/nxnative) commits `9409472`, `b53bb89` (`dm/gfx/nvn/nvn_fstep.h`, `hle_nvn.c` `fstep_cpu_slowed`), adapted | MIT | `games/totk/third_party/licenses/nxnative.txt` |
| lazy clears, uniform read extents, `nvnQueueFinish` without the present wait, upload waits on a condition variable and residency no-op skips in `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`; the address arbiter's SignalToAddress fix in `games/totk/runtime/threads.c` | [nxnative](https://github.com/seedds/nxnative) commits `f5646ef`, `b0a9079`, `df7badd`, `d47f899`, rewritten for this layer | MIT | `games/totk/third_party/licenses/nxnative.txt` |
| `games/totk/runtime/crash_gate.h` (one crash report at a time, with a time limit) and, in `games/totk/runtime/threads.c`, SystemEvent bindings that end with their write handle and signals that wake only the waiters of the signalled object | [nxnative](https://github.com/seedds/nxnative) commits `d47f899` (`dm/host/crash.c`), `49ee623`, `df7badd` (`dm/host/hle_thread.c`), rewritten for this runtime | MIT | `games/totk/third_party/licenses/nxnative.txt` |
| `games/totk/prebuilt/macos-arm64/totk_xlate` (the shader translator, a native program) | built from `vendor/ryujinx` + the patches + `games/totk/tools/totk_xlate` with `publish_native.sh`; contains .NET runtime libraries compiled in by NativeAOT | MIT (Ryujinx, d2r-mobile, nxnative, .NET) | `vendor/ryujinx/LICENSE.txt`, `games/totk/tools/totk_xlate/LICENSE`, `games/totk/third_party/licenses/nxnative.txt` |
| audio effects in `games/totk/runtime/audio_reverb.c`, `games/totk/runtime/audio_effects.c` (delay, reverb, compressor) and the mixer | ported from Ryujinx.Audio | MIT | `games/totk/third_party/licenses/Ryujinx-Audio.txt` |

Fetched or linked at build time (not in this repository):

| Code | Use | License |
|---|---|---|
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) at `aa217ae` | SPIR-V to Metal Shading Language | Apache-2.0 |
| [hactool](https://github.com/SciresM/hactool) at `1d64a83` (with explicit private title-key-file hook), including its bundled mbedTLS | Base + update BKTR merge at build time only | ISC (hactool), Apache-2.0 (mbedTLS); see fetched source LICENSE files |
| libopus (Homebrew), linked statically | Opus audio | BSD-3-Clause |
| libvpx (Homebrew), linked statically | VP9 movies | BSD-3-Clause |
