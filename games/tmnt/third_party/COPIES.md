# Source provenance

## Full-host integration work in progress

- The initial host runtime source files not already present in `runtime/` and
  `third_party/d2r_gfx/` were copied from this repository's
  `games/ssbu/` at commit `9fe3fdc1d08ce5e1a64d120f257d79cc2dc7c0ca`.
  This is a source baseline, not a claim that SSBU-specific behavior is
  correct for TMNT. Runtime policies and graphics behavior require separate
  validation for this game.
  The copied graphics tree retains its adjacent license files.
  TMNT-specific command-buffer splitting, viewport/origin and texture-copy
  changes are port-specific source changes.
  The inherited audio effects, reverb and mixer derive from Ryujinx.Audio;
  its MIT notice is copied to `third_party/licenses/Ryujinx-Audio.txt`.

- `tools/totk_xlate/` is copied from this repository's public
  `games/ssbu/tools/totk_xlate/` at the same commit. The shader compiler's
  Ryujinx source is copied from this repository's `vendor/ryujinx/` tree
  (`4a9223f617daaebef8ab4abee61ffebf71032732` in the local contribution
  base) to `third_party/ryujinx/`, with its MIT `LICENSE.txt`. The TMNT
  patches are applied to a temporary copy at build time; generated shader
  tools and shaders remain outside Git.

- `third_party/spirv-cross/` is byte-identical to this repository's
  `games/unicorn/third_party/spirv-cross/` at the current source baseline.
  That tree records KhronosGroup/SPIRV-Cross commit
  `aa217aeb6c9f0ace7a0ab233b28807edf45eb165` and retains its
  Apache-2.0-or-MIT `LICENSE` and bundled header notices in `LICENSES/`.
  The standalone CLI has a source build path in this game directory.

- The movie decoder integration is port-specific and depends on public libvpx
  and Apple VideoToolbox. Its embedded
  `stb_vorbis.inc` is byte-identical to this repository's
  `games/jamboree/runtime/third_party/stb_vorbis/stb_vorbis.c`; the adjacent
  license is copied to `third_party/stb_vorbis/LICENSE`.

- The offline `bsd:u` Poll response in `runtime/system_services.c` is
  port-specific. It reports invalid socket descriptors without opening a
  network connection.

- `scripts/extract_exefs.py` is copied from `games/dread/scripts/extract_exefs.py` at repository commit `edb17ab9`, with title selection and private-output checks added. The integrity checks remain intact.
- `scripts/extract_romfs.py` is copied from `games/dread/scripts/extract_romfs.py` at the same commit, imports this port's own ExeFS reader and requires a private output directory.
- `requirements.txt` uses the relevant pinned dependencies from `games/dread/requirements.txt` at the same commit; `nsz` is omitted because the current scripts do not support NSZ or updates.
- `third_party/d2r/` is copied from `games/dread/third_party/d2r/` at the same commit; it retains its MIT license.
- The NRO-to-NSO layout in `scripts/prepare_nro.py` derives from `games/ssbu/scripts/rewrite_nros.py` at the same commit, with explicit validation and private-output handling.
- `scripts/module_metadata.py`, `scripts/survey_modules.py` and `tests/test_module_metadata.py` derive from the local `codex/isaac-port` work at commit `549af8c5`, adapted to this game's own rewrite reports. They contain no game data.
- `tests/integration_metadata.py` is copied from the same local commit, with no game data.
- `runtime/relocate.c`, `runtime/relocate.h`, and the relocation probes derive from the same local commit with identifiers renamed; these are standalone host primitives, not game code.
- `runtime/image.c`, `runtime/image.h` and image probes derive from the same local work at commit `c83ff91a`, with identifiers renamed and the integration probe adapted to require this game's rewrite report.
- `tests/integration_loader_primitives.py` derives from the same local work at commit `7290a5fa`, with identifiers renamed and report verification adapted for this game.
- `runtime/gates.c`, `runtime/gates.h` and gate probes derive from the same local work at commit `549af8c5`, with identifiers renamed and report verification adapted for this game.
- `runtime/symbols.c`, `runtime/symbols.h` and its synthetic probe are new; the ELF binding policy was cross-checked against `games/mk8d/runtime/host/loader.c` at `edb17ab9`.
- `runtime/lazy_bind.S` is copied from `games/ssbu/runtime/lazy_bind.S` at repository commit `edb17ab9`; only its introductory comment changed. The accompanying tests are new.
- `ios/tools/gen_embed.py` is adapted from this repository's `games/totk/ios/tools/gen_embed.py` at commit `6bef7003`. It emits private build artifacts only; generated assembly, hashes and game bytes are not committed.
- `ios/tools/gen_nro_embed.py` follows the same signed-section approach for
  this game's one verified rewritten NRO. It emits private game bytes only
  outside the checkout; its synthetic tests contain no game data.
- `tools/totk_xlate/LibExports.cs` and `tools/totk_xlate/spirv_cross_inproc.cpp`
  are copied from this repository's `games/totk/tools/totk_xlate/` at the
  local source base. They connect the same translation service to a statically
  linked iPad app. The SPIRV-Cross sources and their license remain in this
  game's `third_party/spirv-cross/` tree.
- `tools/totk_xlate/publish_ios_lib.sh` follows the corresponding TOTK
  NativeAOT build recipe but writes archives to a caller-selected private
  directory. `ios/tools/build_spirv_cross.py` builds this game's pinned
  SPIRV-Cross sources for iOS; neither script downloads or packages game data.
- `ios/tools/build_deps.py` is adapted from this repository's
  `games/totk/ios/tools/build_deps.py`. It builds pinned source releases of
  Opus 1.6.1, libvpx 1.16.0, Ogg 1.3.6 and Vorbis 1.3.7 into a caller-selected
  private directory. Their upstream license files remain in the downloaded
  source trees; no codec binaries or sources are committed here.

Game files, extracted content, keys, generated shaders, and saves are private and must never enter the repository.
