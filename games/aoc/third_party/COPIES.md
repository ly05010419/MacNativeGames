# Age of Calamity source provenance

The initial copy is `games/jamboree` at MacNativeGames commit
`8c9c71b9503f9b7d9643a4f45aa8fbcb1aff1f02`. Its runtime comes from nxnative
`7861e7c1a9913e020600b1cc7f65132427e9cc50` (MIT). Original notices remain in
the sources and [nxnative/LICENSE](nxnative/LICENSE). No donor folder is read
by the AoC build or runtime.

[jamboree-import.json](jamboree-import.json) records every copied file's original
SHA-256 before AoC adaptations. The donor's original provenance statement is
preserved as [JAMBOREE_COPIES.md](JAMBOREE_COPIES.md); its game-specific claims
apply to that donor. The earlier upstream import manifest and local change patch
remain in `nxnative/` as historical records, not executable build steps.

The target `game.tsv` and `patches.py` were fetched from the same pinned nxnative
revision. [aoc-profile-import.json](aoc-profile-import.json) records their URLs and
original hashes. AoC adaptations are reviewed as source changes:

- Explicit NPDM-confirmed `sys_resource=0`; original timing `fps_target=0`.
- Assertion of all three original instructions displaced by the KIDS step stub.
- Removed donor profile, allocator patch, SDK allocator patch and Bezel movie
  completion override. Retained generic frame release and renderer behavior.
- Direct extracted-directory input mode, full input pins and output inventories,
  no keys, LibHac or container extraction required in that mode.
- Offline Metal compilation, compiler identity in shader/variant caches,
  placeholder rejection and complete-key checks before packaging.
- Geometry-stage storage table at c0 byte 0x410, measured from both original
  AoC geometry shaders; unresolved bindings rejected before packaging.
- AoC app identity, external writable state, session logs and profile lock.

Shader maps, facts, pairs, uniform proof tables and libraries are generated from
private AoC originals in the external workspace. No donor game shader list or
cache is used. The prebuilt shader translator's exact file set and SHA-256 values
remain unchanged in `prebuilt/macos-arm64/SHA256SUMS`. It contains shader tools,
no CPU backend or Horizon service implementation. LibHac remains an optional,
SHA-256-pinned local prerequisite for the inherited container importer only.

See [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) for transitive licenses.

AoC dynamic-FPS/stats work (2026-10-27): the compact HUD, independent overlay
controls and timing history follow the local TOTK frontend's UI pattern, with
AoC-specific AppKit views and telemetry. No TOTK game addresses or timing patches
were imported. AoC 1.3.0's four timing sites assert the original instructions;
its KIDS scheduler and scene/battle use independently measured intervals. The
scene counter uses native CNTFRQ_EL0, independently checked against Mach time
(the M5 exposes different native-counter and mach_absolute_time ABI rates).
