# Source copies and update policy

The game is isolated as required by the root CONTRIBUTING.md. It does not build from, import, execute, or copy tooling from another game's directory.

## Imported source

Base: [seedds/nxnative](https://github.com/seedds/nxnative/tree/7861e7c1a9913e020600b1cc7f65132427e9cc50), commit **7861e7c1a9913e020600b1cc7f65132427e9cc50**, MIT. Imported on 2026-10-25. The original license is [nxnative/LICENSE](nxnative/LICENSE). Original authors retain their copyrights; this contribution does not claim authorship of the imported host or translator.

| Destination in this game | Original folder at that commit | Adaptation |
|---|---|---|
| `runtime/host` | `dm/host` | NRO capacity, movie continuation/frame release, Mac controller configuration, explicit arm64/macOS 26 build flags |
| `runtime/gfx` | `dm/gfx` excluding prebuilt binaries and the duplicate C# translator sources | Discard vertex side effects, reflection flags, write tracking, `.bea` reader, compiler adapter, GPU regressions |
| `runtime/tools` | `dm/tools`, excluding upstream's multi-game tests and Ghidra tool | External-profile patch discovery and the asserted SDK build patch; synthetic discard test added |
| `runtime/ios/tools` | `dm/ios/tools` | Carried as build-time imports and chain-version inputs; no iOS app target is offered |
| `runtime/games/{game.py,profile_guess.py}` | `dm/games` | Profile helpers, no other game's profile copied |
| `runtime/third_party` | `dm/third_party`, excluding FLIP and the duplicate Ryujinx shader sources | Original licenses retained; dependency README adjusted for the subset |
| `prebuilt/macos-arm64/dmsh_xlate` | `dm/gfx/tools/dmsh_xlate_bin` | Binary-only tooling, 12 retained files byte-identical; `.pdb` and Humanizer satellite resources removed; exact hashes in `SHA256SUMS` |
| `third_party/nxnative/build_ipa_from_nsp.py` | `scripts/build_ipa_from_nsp.py` | Strict title/version/hash gates, runtime-MSL output support, caller PATH precedence, explicit local-only NuGet restore |

[nxnative/import-manifest.json](nxnative/import-manifest.json) records the copied source paths, destination paths and SHA-256 values **at import time**, before this directory's adaptations. [nxnative/local-changes.patch](nxnative/local-changes.patch) records the tested bring-up changes relative to that pinned nxnative commit. It is a provenance record, not a build step: all final source is already present in `runtime/`. Later target-specific packaging and UI adaptations are described here and in the target README and reviewed as ordinary source in this PR.

The game profile and `patches.py`, original-input hash manifest, Cocoa bundle launcher, portable wrapper, local tests, the independent Mac settings panel/HUD and output-cap helper, and documentation were added for this contribution. Shader map/facts/pairs are identifiers and binding metadata only. The map has four columns (generated package, index, stage, key); its original-resource path column is removed both from the public table and its generator. Package grouping uses a generic root rule, and the extraction marker is the atomic output directory rather than an original asset path. Generated table headers retain their historical “provisional profile” label; that label is not a claim of complete game validation. `c1_full.tsv` and `cb_extent.tsv` are generated privately from each shader build and are not checked in.

## Transitive dependencies

See [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). The binary-only Ryujinx dependency contains `Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator`; it does not contain a CPU backend or Horizon service implementation. LibHac is a build-time content-format reader used only by `nx_extract`, never a runtime service layer. The compiled shader tooling is not a game shader library. The matching translator source is referenced at the pinned upstream commit, not duplicated here. libvpx is retained as source because the host compiles it for movie playback; it is not duplicated by the translator binary. LibHac is an explicit local package prerequisite with a pinned SHA-256, never downloaded by the build or redistributed in this target.

## Updating

Update copies explicitly inside this game. Record the new source commit and hashes, preserve licenses, regenerate the chain header if a chain source changes, rebuild the full shader set from private original inputs, and rebuild the host. Re-run synthetic tests, Metal regressions and the documented in-game scenes. Do not reuse an old translated library after changing the chain or its game-input digest.

A fixed-address game or SDK patch must name the original NSO build ID and assert its original instruction. Unsupported game builds must stop rather than inherit this profile's addresses. Never commit original/extracted game data, rewritten modules, translated shader code/libraries, keys, saves or game screenshots.
