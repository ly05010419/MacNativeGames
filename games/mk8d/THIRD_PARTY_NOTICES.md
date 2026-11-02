# Mario Kart 8 Deluxe third-party notices

These notices live inside the isolated game directory, as the repository's contribution rules ask. Original license files and source notices are retained. The additions made for this target use the repository's MIT license.

| Component | Source / version | License and notice |
|---|---|---|
| Native host, NVN/Metal layer, AOT rewriter, shader tools, Mac app packer, build orchestrator, the game profile and its notes | [nxnative](https://github.com/seedds/nxnative), commit `062cd06e6fd79ec0bcd52d59f778fd6438994a2a`, modified as listed in [COPIES.md](third_party/COPIES.md) | MIT, copyright 2026 seedds; [license](third_party/nxnative/LICENSE) |
| Build wrapper, input pins, tests and documentation pattern | MacNativeGames `games/jamboree` (Microbai, PR #684), at MacNativeGames commit `5ab07b7` | MIT, copyright 2026 MacNativeGames contributors; [license](../../LICENSE) |
| `nvn.h` reconstructed API types | nxnative `dm/gfx/nvn/include/nvn.h` | MIT, copyright 2020 h1k421; full notice in the [header](runtime/gfx/nvn/include/nvn.h) |
| Texture layout size algorithm, adapted | [deko3d](https://github.com/devkitPro/deko3d), `source/dk_image.cpp` | zlib, copyright 2018–2020 fincs; [license](third_party/licenses/deko3d.txt). Adaptation marked in `runtime/gfx/nvn/nvn_tex.c` |
| GPU index widening, clear triangles, vertex counter lowering | MacNativeGames, through the pinned nxnative source | MIT, copyright 2026 MacNativeGames contributors; [license](runtime/third_party/macnativegames/LICENSE) |
| VP9 decoder subset | libvpx v1.17.0, commit `6df3ec3` | BSD 3-Clause; [license](runtime/third_party/libvpx/LICENSE), [patent grant](runtime/third_party/libvpx/PATENTS), [source record](runtime/third_party/libvpx/README.md) |
| Vorbis decoder | stb_vorbis 1.22, commit `1ee679c` | MIT or public domain; [license](runtime/third_party/stb_vorbis/LICENSE) |
| Vulkan header subset | Vulkan-Headers 1.4.363 | Apache-2.0 or MIT; [license](runtime/third_party/vulkan-headers/LICENSE.md); per-file SPDX notices retained |
| SPIR-V to MSL converter (built at packaging) | [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) `vulkan-sdk-1.4.363.0`, commit `f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7`, compiled by `scripts/build_mk8d.py` | Apache-2.0 (some files MIT / Khronos); [license](prebuilt/macos-arm64/spirv_cross/LICENSE), [license texts](prebuilt/macos-arm64/spirv_cross/LICENSES) |
| Shader translation libraries only (binary) | Ryujinx 1.3.2 shader subset, with nxnative modifications | MIT; [license](third_party/licenses/ryujinx.txt), [upstream modifications](third_party/licenses/ryujinx-build.md) |
| SPIR-V generator (binary) | Spv.Generator, carried with the Ryujinx shader subset | MIT; [license](third_party/licenses/spv-generator.txt) |
| Gommon (binary) | [GreemDev/Gommon](https://github.com/GreemDev/Gommon), NuGet 2.8.1.2, translator dependency | MIT; [license](third_party/licenses/gommon.txt) |
| Humanizer, core assembly only (binary) | [Humanizr/Humanizer](https://github.com/Humanizr/Humanizer), NuGet 3.0.10, translator dependency | MIT; [license](third_party/licenses/humanizer.txt) |
| Microsoft.IO.RecyclableMemoryStream (binary) | NuGet 3.0.1, translator dependency | MIT; [license](third_party/licenses/recyclablememorystream.txt) |
| MsgPack.Cli (binary) | [msgpack/msgpack-cli](https://github.com/msgpack/msgpack-cli), NuGet 1.0.1, translator dependency | Apache-2.0, copyright 2010–2016 FUJIWARA, Yusuke; [license](third_party/licenses/msgpack-cli.txt) |
| System.CodeDom (binary) | .NET, NuGet 4.4.0, translator dependency | MIT; [license](third_party/licenses/system.codedom.txt) |
| LibHac | NuGet 0.19.0; an explicit SHA-256-pinned local package prerequisite, restored offline only, not vendored | MIT, copyright 2018–2022 Alex Barney; [notice](third_party/licenses/libhac.txt) |

The exact translator dependency versions are recorded in `prebuilt/macos-arm64/dmsh_xlate/dmsh_xlate.deps.json`; binary checksums are in `prebuilt/macos-arm64/SHA256SUMS`. No emulator CPU backend, emulator service layer, game program, RomFS, original or translated game shader, firmware file or game asset is distributed with this contribution.

Python packages, .NET, Xcode and LibHac are user-installed build tools and are not redistributed here. Generated fonts, shader tables and all other game-derived build outputs stay outside source control.
