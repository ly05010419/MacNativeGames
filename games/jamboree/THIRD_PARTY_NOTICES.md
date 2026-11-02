# Jamboree third-party notices

These notices live inside the isolated game directory to comply with the repository rule that only a root README history line changes outside it. Original license files and source notices are retained. The game-specific additions use the repository's MIT license.

| Component | Source / version | License and notice |
|---|---|---|
| Native host, NVN/Metal layer, AOT rewriter, shader tools, build orchestrator | [nxnative](https://github.com/seedds/nxnative), commit `7861e7c1a9913e020600b1cc7f65132427e9cc50`, modified as listed in [COPIES.md](third_party/COPIES.md) | MIT, copyright 2026 seedds; [license](third_party/nxnative/LICENSE) |
| `nvn.h` reconstructed API types | nxnative `dm/gfx/nvn/include/nvn.h` | MIT, copyright 2020 h1k421; full notice in [header](runtime/gfx/nvn/include/nvn.h) |
| Texture layout size algorithm, adapted | [deko3d](https://github.com/devkitPro/deko3d), `source/dk_image.cpp` | zlib, copyright 2018–2020 fincs; [license](third_party/licenses/deko3d.txt). Adaptation marked in `runtime/gfx/nvn/nvn_tex.c` |
| GPU index widening, clear triangles, vertex counter lowering | MacNativeGames, through the pinned nxnative source | MIT, copyright 2026 MacNativeGames contributors; [license](runtime/third_party/macnativegames/LICENSE) |
| VP9 decoder subset | libvpx v1.17.0, commit `6df3ec3` | BSD 3-Clause; [license](runtime/third_party/libvpx/LICENSE), [patent grant](runtime/third_party/libvpx/PATENTS), [source record](runtime/third_party/libvpx/README.md) |
| Vorbis decoder | stb_vorbis 1.22, commit `1ee679c` | MIT or public domain; [license](runtime/third_party/stb_vorbis/LICENSE) |
| Vulkan header subset | Vulkan-Headers 1.4.363 | Apache-2.0 or MIT; [license](runtime/third_party/vulkan-headers/LICENSE.md); per-file SPDX notices retained |
| Shader translation libraries only | Ryujinx 1.3.2 shader subset, with nxnative modifications | MIT; [license](third_party/licenses/ryujinx.txt), [upstream binary modifications](third_party/licenses/ryujinx-build.md) |
| SPIR-V generator | Spv.Generator, carried with the Ryujinx shader subset | MIT; [license](third_party/licenses/spv-generator.txt) |
| Gommon | [GreemDev/Gommon](https://github.com/GreemDev/Gommon), NuGet 2.8.1.2, compiler dependency | MIT; [license](third_party/licenses/gommon.txt) |
| Humanizer (core assembly only) | [Humanizr/Humanizer](https://github.com/Humanizr/Humanizer), NuGet 3.0.10, compiler dependency | MIT; [license](third_party/licenses/humanizer.txt) |
| Microsoft.IO.RecyclableMemoryStream | [Microsoft.IO.RecyclableMemoryStream](https://github.com/microsoft/Microsoft.IO.RecyclableMemoryStream), NuGet 3.0.1, compiler dependency | MIT; [license](third_party/licenses/recyclablememorystream.txt) |
| MsgPack.Cli | [msgpack/msgpack-cli](https://github.com/msgpack/msgpack-cli), NuGet 1.0.1, compiler dependency | Apache-2.0, copyright 2010–2016 FUJIWARA, Yusuke; [license](third_party/licenses/msgpack-cli.txt) |
| System.CodeDom | .NET, NuGet 4.4.0, compiler dependency | MIT; [license](third_party/licenses/system.codedom.txt) |
| LibHac | NuGet 0.19.0, package source commit `cd4004af194a51712812c680f0a028ba9152c0f8`; explicit SHA-256-pinned local package prerequisite; offline restore only, not vendored | MIT, copyright 2018–2022 Alex Barney; [notice](third_party/licenses/libhac.txt); package metadata declares MIT |

The exact shader compiler dependency versions are recorded in `prebuilt/macos-arm64/dmsh_xlate/dmsh_xlate.deps.json`; binary checksums are in `prebuilt/macos-arm64/SHA256SUMS`. No emulator CPU backend, emulator service layer, game program, RomFS, original/translated game shader or game asset is distributed with this contribution.

Python packages, .NET, Xcode and SPIRV-Cross are user-installed build tools and are not redistributed here. The generated icon font uses the user's installed macOS font locally; generated fonts and all game-derived build outputs stay outside source control.
