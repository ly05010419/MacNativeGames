# Worldend Syndrome source provenance

The initial copy is `games/jamboree` at MacNativeGames commit
`92ec79bd10c006e1cb7ca32d7ac1d8f66c758a15`. Its runtime comes from nxnative
`7861e7c1a9913e020600b1cc7f65132427e9cc50` (MIT). Original notices remain in
the sources and [nxnative/LICENSE](nxnative/LICENSE). No donor folder is read
by the Worldend build or runtime.

[jamboree-import.json](jamboree-import.json) records every copied file's original
SHA-256 before Worldend adaptations. The donor's original provenance statement is
preserved as [JAMBOREE_COPIES.md](JAMBOREE_COPIES.md); its game-specific claims
apply to that donor. The earlier upstream import manifest and local change patch
remain in `nxnative/` as historical records, not executable build steps.

Worldend adaptations are reviewed as source changes:

- New profile `runtime/games/010048900CF64000` (title ID, base v0 / 1.0.0,
  NPDM-confirmed `sys_resource=0`, `mem_used=0x4A00000`, `device_api=53.399`,
  touch + handheld pad layout, Unity `Data/` shader group). No donor shader map,
  facts, pairs or game patches were kept. Worldend's 543-key map is from a
  private split of this title.
- Empty `patches.py` until a first boot measures a needed site. SDK 6.4.0 has no
  matching cut in the inherited `sdk_patches.py` table (those are other builds).
- Builder, launcher, bundle ID, save directory and title gate retargeted from
  Jamboree 2.3.0 / `v458752` to Worldend Syndrome 1.0.0 / `v0`.
- First shader split may write `shader_map.tsv` / facts / pairs when the profile
  does not yet have them (bring-up only; later builds still check committed tables).
- Languages restricted to the NACP set: `en-US`, `en-GB`.

Shader maps, facts, pairs, uniform proof tables and libraries are generated from
private Worldend originals in the external workspace. No donor game shader list or
cache is used. The prebuilt shader translator's exact file set and SHA-256 values
remain unchanged in `prebuilt/macos-arm64/SHA256SUMS`. It contains shader tools,
no CPU backend or Horizon service implementation. LibHac remains an explicit,
SHA-256-pinned local prerequisite for the container importer.

See [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) for transitive licenses.
