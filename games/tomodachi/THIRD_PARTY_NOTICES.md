# Tomodachi dependencies

The title and its copies of the parent runtime remain MIT-licensed under the
repository LICENSE. Imported code retains the following notices and licenses.
All paths below are local to this game folder.

| Component | Source | License / retained file |
|---|---|---|
| NSO rewriter | d2r-mobile, via the parent project | MIT, `third_party/d2r/LICENSE` |
| NVN/Metal graphics | d2r-mobile and parent contributors | MIT, `third_party/d2r_gfx/LICENSE` |
| Texture layout metadata | deko3d | zlib, `third_party/d2r_gfx/LICENSE.deko3d` |
| BC texture decoder | bcdec | MIT/public domain, `third_party/d2r_gfx/third_party/bcdec/LICENSE` |
| Shader/common/SPIR-V translator source | Ryujinx | MIT, `third_party/ryujinx/LICENSE.txt` |
| Shader translation service | d2r-mobile and parent contributors | MIT, `tools/totk_xlate/LICENSE` |
| NativeAOT translator binary | above translator plus .NET runtime | MIT, retained translator/Ryujinx licenses; see `prebuilt/macos-arm64/README.md` |
| Audio effect arithmetic | Ryujinx.Audio, via parent | MIT, `third_party/licenses/Ryujinx-Audio.txt` |
| Container/extraction helpers | Tyraelone/MacNativeGames, via parent | MIT, `tools/LICENSE`, revision details in `tools/UPSTREAM.json` |
| SPIRV-Cross (external input tool) | KhronosGroup/SPIRV-Cross at `aa217ae` | Apache-2.0, license in its external source checkout |
| libopus and libvpx (system build dependencies) | Homebrew upstream distributions | BSD-3-Clause, licenses in the installed source/distribution |

Copy locations, input hashes, and title adaptations are recorded in
`third_party/COPIES.md` and `third_party/COPIES.json`. Translated game shaders
and extracted artwork are never distributed here.
