# Skyward Sword HD source provenance

Three sources, each named per file in [ss-import.json](ss-import.json) (`origins`), with the files kept as they
were (SHA-256), the ones changed here (SHA-256 and line count before and after) and the ones left out.
`tests/test_provenance.py` checks the folder against it; large changed files carry the owner's growth limit
(Albert, 2026-11-11). No donor folder is read by this build, its runtime or its tests.

- **The target layer** (`scripts/`, `app/`, `assets/`, `tests/`, the settings window, the corner box and the shader
  list export in `runtime/host`): `games/la` at MacNativeGames `549b8a9a`, renamed for this game.
- **The runtime** (`runtime/`): nxnative `b1dee89c` (MIT), the commit that added this game's profile, with the file
  selection `games/la` made from its own pin (no other game's profile, no iOS app sources, the translator and
  Ryujinx sources left out for the binaries in `prebuilt/`). Its graphics and host tests are included whole.
- **The runtime changes of this repository** (as `games/la` and `games/aoc` carry them over nxnative `7861e7c1`):
  merged three ways onto `b1dee89c` (base `7861e7c1`, ours `games/la/runtime`, theirs `b1dee89c`), conflicts
  resolved by hand; the review is in [docs/validation.md](../docs/validation.md#tests).

The game profile (`game.tsv`, `dual_want.tsv`, `bf_want.tsv`, `pso_manifest.tsv`) was fetched unchanged from
nxnative `b1dee89c`; [ss-profile-import.json](ss-profile-import.json) records the URLs and hashes.
`nxnative/build_ipa_from_nsp.py` is nxnative's at `b1dee89c` with this repository's extracted mode.

Changes from the sources, reviewed as source changes:

- The importer takes only an extraction checked by `scripts/extract_ss.py`, for 1.0.1 (v65536) only, in the
  environment `scripts/ss_env.py` builds; its container mode stops at once. `nx_extract` is `games/la`'s (programs
  chosen by their content metas, `Select.cs`), set to this title and version.
- No outside profile or rewrite rule takes part: `game.py` and `patch_nso.py` read no `DM_GAMES_DIR`
  (nxnative `b1dee89c` added that loading to `patch_nso.py`).
- Kept from this repository's runtime: the settings window, corner box and statistics (`aoc_stats.c`), the
  shader list export (its pipeline key size set to this runtime's 440 bytes), Age of Calamity's timing hooks
  (inert without a frame step), the feedback-snapshot flag, offline Metal only, shader maps without source paths,
  quieter logs and the long-frame log off by default.
- Taken from nxnative instead of this repository's older copies: presentation pacing (the display-link hold
  keeps nxnative's 25 ms threshold: the hold for every period broke the present allocators), the movie decoder's
  slot handling, scripted input and the rasterizer-discard path.
- macOS 26.0 as the minimum (nxnative 26.6); MetalFX's macOS 27 rectangle offsets only with that SDK.
- Weak definitions of `g_flag` (`hle_nvn.c`) and `aoc_timing_apply` (`loader.c`) for the unit tests that link part
  of the host; the shader-chain header regenerated for this tree.
- Tests: the copied ones rewritten where they encoded Link's Awakening's facts (its physics cut, cartridge
  exception, excepted shaders, warm-up seed, scripted-press wait); nxnative's own tests adjusted only for the folder
  layout (its `dm/` is `runtime/`) and the one profile here.
