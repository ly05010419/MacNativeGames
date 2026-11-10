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

Warm-up seed without repeats (2026-11-10): `scripts/pso_seed.py` drops an R row whose key, normalized as the host does
(`pso_key_norm`), repeats an earlier one (`scripts/pso_skey_layout.m` reports the offsets), and the seed made again from its
four sources loses the 98 such rows the host already skipped while warming up; nothing else in it changes.

Warm-up compile workers (2026-11-10): `runtime/gfx/nvn/nvn_mtl.m`'s warm-up reads, deduplicates and prepares the list's
render pipelines on its thread and compiles them on two workers, each pipeline handed to the game as soon as it is made;
`DM_PSO_WARM_THREADS=1` compiles them one at a time, as before.

Pipeline phase 1 (2026-11-09):
- `runtime/host/hle_hid.c`, `runtime/host/tests/t_hid.c`, `tests/test_hid_input.py`: scripted
  presses wait until the game sees each release, ported from Link's Awakening's fix
  (MacNativeGames `1348dd06`, the same code; only the test's title id differs).
- `runtime/gfx/nvn/nvn_pso_norm.h` (new): `pso_key_norm`, copied unchanged from nxnative
  `062cd06e` (MIT, `games/mk8d/runtime/gfx/nvn/nvn_pso_skey.h`), plus the warm-up's row counts
  (new). `nvn_pso_skey.h` normalizes parsed rows, `nvn_mtl.m` normalizes live keys, skips a
  render row whose normalized key it already met and logs the counts per list; the unused
  `mtl_warm_wait` is removed (and its declaration in `nvn_obj.h`).
- `runtime/gfx/nvn/hle_nvn.c`: a one-time warning for an alpha test other than always or a logic
  op other than copy, which the host only records.
