# Mario Kart World (experimental port)

This folder contains an early Apple silicon Mac port of Mario Kart World 1.0.0 (program ID `0400c3f00006e000`). It is a self-contained fork of the Wonder runtime, rewriter and graphics layer. No game content is included; development requires your own game dump and system files.

The game has been observed starting through graphics, audio and input initialization and submitting its first draw. **It is not playable:** the Switch 2 NVN2 shader format and GPU instructions still need a translator. Shared fonts and a complete build workflow are also unfinished. The existing `scripts/build_mkw.py` is an unadapted Wonder script and is deliberately disabled. `assets/expected-v1.0.0.json` records hashes from the supplied MKW NSP and the existing development rewrite; no build script checks them yet. The 1.2.1 metadata and shader list are inherited from Wonder and do not describe MKW.

## Development

The runtime targets macOS on Apple silicon and uses Xcode command line tools and Homebrew's `opus` and `libvpx`. `make binary` builds `build/mkw-native` without game data. `make runtime` additionally requires locally prepared MKW images, rewriter validation output, RomFS index and shared-font manifest. `make boot` runs that prepared runtime. These targets are development tools, not a supported end-to-end installation procedure.

`scripts/extract_mkw.py` extracts the ExeFS and indexes the RomFS from an NSP containing **unencrypted** NCAs; it verifies the filesystem hash trees and reads the RomFS in place from the NSP. It does not decrypt a normal encrypted dump or verify the NCA signature. Run `python3 scripts/extract_mkw.py --help` for its arguments. Keep extracted game data and the source NSP private.

For shader investigation, `MKW_SHADER_DUMP=<n>` logs the first `n` shader builder calls; `MKW_SHADER_DIR=<existing-directory>` saves observed control sections and code there. `MKW_NVN2_PROBE=1` logs real-driver function offsets, optionally selecting names with the comma-separated `MKW_NVN2_PROBE_NAMES`. These are diagnostics for a prepared development runtime; their output can contain game data and should remain private.

`tools/prepare_ampere_jobs.py <capture-dir> <output-dir>` prepares private captures for the SM86 frontend in [nxnative](https://github.com/seedds/nxnative) at commit `65501fbbaf8526f7aab0a1edd028135888590f8e`. Run its `dmsh_xlate` with the generated `jobs.json` and an output directory. Captures from the older dumper may be rejected because they omit the c1 constant segment; capture again with the current runtime. These offline jobs do not supply the runtime's resource bindings or make the game playable.
