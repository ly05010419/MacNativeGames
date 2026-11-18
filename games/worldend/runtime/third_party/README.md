# dm/third_party: third-party code

This is code written by others, shipped with the repository so there is nothing to download. Each component keeps its original license, and the license file sits in its own directory.

| Directory | What it is | Version and changes | Where it is used | License file |
|---|---|---|---|---|
| `libvpx/` | The VP9 decoder from libvpx: the 71 source files a VP9-decoder-only build compiles, their headers, and the generated configuration in `config/` | v1.17.0 (upstream commit `6df3ec3`), unmodified | Host: decodes the games' VP9 movies (`dm/host/hle_movie.c`). `dm/host/Makefile` builds it into `gen/vpx/libvpx.a`; the iOS build compiles the same files | `libvpx/LICENSE` (BSD 3-Clause), with the patent grant in `libvpx/PATENTS` |
| `stb_vorbis/` | Ogg Vorbis decoder, a single file `stb_vorbis.c` | 1.22 (upstream commit `1ee679c`), unmodified | Host: decodes the Vorbis sound tracks of the games' WebM movies (`dm/host/hle_movie.c`, which includes the file with its push-data API only) | `stb_vorbis/LICENSE` (MIT or public domain, your choice; the same text ends `stb_vorbis.c`) |
| `vulkan-headers/` | The Vulkan C headers: `vulkan_core.h`, `vk_platform.h`, `vulkan_vi.h` and the `vk_video/` headers they include | Vulkan-Headers 1.4.363, unmodified | Host: the Vulkan layer on Metal (`dm/gfx/vk/hle_vk.m`) that runs Hades | `vulkan-headers/LICENSE.md` (Apache-2.0 or MIT, your choice) |

A few more notes:

- `libvpx/README.md`, `stb_vorbis/README.md` and `vulkan-headers/README.md` record where each came from and what it is used for.
- Four pieces of third-party code live outside this directory: `dm/gfx/nvn/include/nvn.h` (MIT, author named in the file header), the layout size algorithm in `dm/gfx/nvn/nvn_tex.c`, ported from deko3d (zlib license), and two adapted from MacNativeGames (MIT, license text in `macnativegames/LICENSE`): in `dm/gfx/nvn/nvn_mtl.m` the GPU widening of 8-bit indices (`wide_*`) and the in-pass clear triangles (`clear_color_tri` / `clear_ds_tri`), from its `widen_u8_gpu` and `clear_quad` / `clear_ds_quad`, and the Tears of the Kingdom visibility-counter lowering in the pinned binary translator (`VertexCounterLowering`; source is referenced upstream, not duplicated here).
