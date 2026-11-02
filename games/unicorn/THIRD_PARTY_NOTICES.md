# Third-party code and copies

All paths below are relative to `games/unicorn`. No build step reads another game's folder.

The runtime, graphics layer, rewriter and extraction tools were copied from MacNativeGames
`games/by3` at commit `30a1b93`, then adapted for Unicorn Overlord 1.05. The optional
`third_party/d2r/equiv.py` verifier uses Unicorn outside gameplay; the runtime has no CPU emulator.

| Code | Source | License | Local notice |
|---|---|---|---|
| `third_party/d2r`, `third_party/d2r_gfx` | d2r-mobile, via MacNativeGames/Bayonetta 3 | MIT | `third_party/d2r/LICENSE`, `third_party/d2r_gfx/LICENSE` |
| NVN texture layout | deko3d, via the graphics copy | zlib | `third_party/d2r_gfx/LICENSE.deko3d` |
| `third_party/d2r_gfx/third_party/bcdec` | bcdec 0.985 | MIT or public domain | `third_party/d2r_gfx/third_party/bcdec/LICENSE` |
| Audio effects, reverb and mixer | Ryujinx.Audio, via MacNativeGames | MIT | `third_party/licenses/Ryujinx-Audio.txt` |
| `prebuilt/macos-arm64/totk_xlate` | Unmodified NativeAOT translator copied from `games/by3/prebuilt/macos-arm64` at `30a1b93` | MIT (Ryujinx, d2r-mobile, .NET) | `third_party/licenses/Ryujinx-Shader.txt`, `third_party/licenses/d2r-translator.txt` |
| `third_party/spirv-cross` | [KhronosGroup/SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross/tree/aa217aeb6c9f0ace7a0ab233b28807edf45eb165), commit `aa217aeb6c9f0ace7a0ab233b28807edf45eb165`; unchanged CLI sources and headers | Apache-2.0 or MIT; bundled SPIR-V headers retain their notices | `third_party/spirv-cross/LICENSE`, `third_party/spirv-cross/LICENSES/` |

`totk_xlate.sha256` checks the bundled translator before copying it to the work folder.
SPIRV-Cross is compiled from the bundled source; neither tool is fetched during the build.

Installed separately before building:

| Library | Use | License |
|---|---|---|
| libopus (Homebrew) | Statically linked Opus audio | BSD-3-Clause |
| libvpx (Homebrew) | Statically linked VP9 movies | BSD-3-Clause |

Python dependencies and their versions are listed in `requirements.txt`. Install them before an offline build.
