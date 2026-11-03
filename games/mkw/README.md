# Mario Kart World (experimental port)

This folder contains an early Apple silicon Mac port of Mario Kart World 1.0.0 (program ID `0400c3f00006e000`). It is a self-contained fork of the Wonder runtime, rewriter and graphics layer. No game content is included; development requires your own game dump and system files.

The game has been observed starting through graphics, audio and input initialization and submitting its first draw. **The native port is not playable:** the Switch 2 NVN2 shader format and GPU instructions still need a translator. Shared fonts and a complete build workflow are also unfinished. The existing `scripts/build_mkw.py` is an unadapted Wonder script and is deliberately disabled. `assets/expected-v1.0.0.json` records hashes from the supplied MKW NSP and the existing development rewrite. The shader list is inherited from Wonder and does not describe MKW.

## Development

The runtime targets macOS on Apple silicon and uses Xcode command line tools and Homebrew's `opus` and `libvpx`. `make binary` builds `build/mkw-native` without game data. `make runtime` additionally requires locally prepared MKW images, rewriter validation output, RomFS index and shared-font manifest. `make boot` runs that prepared runtime. These targets are development tools, not a supported end-to-end installation procedure.

The runtime now decodes NVN2 control sections and prepares `arch: sm86` requests, including c1 constants and compute layout metadata. It records graphics/compute push constants separately and exposes them at shader constant bank 24. The shader cache has its own MKW identity. The development bridge in `tools/ampere_service` connects nxnative's batch frontend to the runtime protocol; see its README for build and environment settings. `MKW_AMPERE_SERVICE` selects that executable, and without it startup stops explicitly rather than using the Maxwell translator. This variable does not accept the batch-only `dmsh_xlate` executable directly.

The control-layout check can be built with `xcrun clang -Wall -Wextra -Werror tests/nvn2_shader.c -o /tmp/mkw-control-test`; run it without arguments for synthetic cases, or pass pairs of `.ctl` and `.bin` captures to check their lengths and stages.

`scripts/extract_mkw.py` extracts the ExeFS and indexes the RomFS from an NSP containing **unencrypted** NCAs; it verifies the filesystem hash trees and reads the RomFS in place from the NSP. It does not decrypt a normal encrypted dump or verify the NCA signature. Run `python3 scripts/extract_mkw.py --help` for its arguments. Keep extracted game data and the source NSP private.

For comparison, [Eglantine](https://github.com/Djihads80/Eglantine) build 135 for macOS displayed the game's title artwork on the development Mac, at about 9–12 FPS while starting; gameplay was not verified. To prepare a separate loose dump for Eglantine from an extraction made by `extract_mkw.py`, run:

```sh
python3 scripts/prepare_eglantine.py --exefs <extracted-exefs> --index <extracted-romfs/index.json> --out <new-private-directory>
```

The script checks the ExeFS and RomFS hashes against the recorded 1.0.0 image and writes `exefs/main` and `exefs/game.romfs` for Eglantine. It needs about 23 GB of free space; the output contains game data and must not be shared or committed. Eglantine is a separate GPL-licensed emulator, not this native port.

For shader investigation, `MKW_SHADER_DUMP=<n>` logs the first `n` shader builder calls; `MKW_SHADER_DIR=<existing-directory>` saves observed control sections and code there. `MKW_NVN2_PROBE=1` logs real-driver function offsets, optionally selecting names with the comma-separated `MKW_NVN2_PROBE_NAMES`. These are diagnostics for a prepared development runtime; their output can contain game data and should remain private.

`tools/prepare_ampere_jobs.py <capture-dir> <output-dir>` prepares private captures for the SM86 frontend in [nxnative](https://github.com/seedds/nxnative) at commit `65501fbbaf8526f7aab0a1edd028135888590f8e`. Run its `dmsh_xlate` with the generated `jobs.json` and an output directory. Captures from the older dumper may be rejected because they omit the c1 constant segment; capture again with the current runtime. These offline jobs do not supply the runtime's resource bindings or make the game playable.


The native image-binding path now retains writable identity-swizzle views separately from sampling views. The storage-view GPU probe passes 15,689 checks, including actual image writes to a selected mip/layer. A bounded full-game GPU run cleared the prior compute/image failures and stopped at the explicit task/mesh replay guard; task/mesh translation and execution remain required for playability. See [HANDOFF.md](HANDOFF.md).
