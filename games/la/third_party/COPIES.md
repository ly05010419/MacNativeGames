# Link's Awakening source provenance

The initial copy is `games/aoc` at MacNativeGames commit `ed30a81db3f47e0d44cf2259a393cce48fd873e2`
(the folder as it is on MNG `main`). Its runtime comes from nxnative
`7861e7c1a9913e020600b1cc7f65132427e9cc50` (MIT) through `games/jamboree` and `games/aoc`.
Original notices remain in the sources and [nxnative/LICENSE](nxnative/LICENSE). No donor folder is
read by this build, its runtime or its tests.

[la-import.json](la-import.json) records every copied file: the ones kept as they were (with their
SHA-256), the ones changed here (SHA-256 and line count before and after), and the AoC files left out.
`tests/test_provenance.py` checks the folder against it. `nxnative/import-manifest.json` and
`nxnative/local-changes.patch` are the donor's historical records, kept unchanged; they are not build steps.

The game profile (`game.tsv`, `patches.py`, `shader_facts_manual.tsv`, `bf_want.tsv`) was fetched
unchanged from the same pinned nxnative revision; [la-profile-import.json](la-profile-import.json)
records the URLs and hashes.

Changes from the copy, reviewed as source changes:

- The importer takes only an extraction checked by `scripts/extract_la.py`, for 1.1.0 (v131072) only,
  and only in the environment `scripts/la_env.py` builds; its container mode stops at once.
- `patch_nso.py` and `game.py` no longer read `DM_GAMES_DIR` / `DM_GAMES_FIRST`: no outside profile
  or rewrite rule takes part.
- `nx_extract` picks the base game and the update by their content metas (`Select.cs`), checks header
  signatures and each program's SHA-256, and stops when the RomFS cannot be opened. One base meta with
  an invalid signature is tolerated, named by SHA-256 in `assets/integrity-exceptions.json`.
- App identity, launcher and settings title for Link's Awakening. AoC's frame timing code stays in the
  runtime: this profile has no frame step, so its state page is never installed, but a nonzero target
  still sets the present period (`hle_nvn.c`): 30 caps the game at 30 FPS, 60 ignores its interval 2.
- The settings window, shortcut hints and keys 2 and 4 no longer offer AoC's FPS target and dynamic step
  (`front_settings.m`, `front_shortcuts.h`, `tests/settings_preview.m`), and `front_mac.m` no longer
  loads or saves a target in the settings file, so one saved by an earlier build cannot stay in force.
  `DM_FPS_TARGET` and `DM_DYNAMIC_FPS` still set them for diagnostics.
- One corner box, top left, after Tears of the Kingdom's compact HUD, replaces AoC's shortcut hints and stats pane (new
  `runtime/host/front_hud.m` / `.h` and `hud_settings.h`; `front_settings.m`, `front_settings.h`, `front_shortcuts.h`,
  `front_mac.m`, the host `Makefile`, `scripts/test_settings.py`, `tests/settings_preview.m`). Key 1 shows or hides it and
  keeps its lines; keys 2-6 add or remove FPS, the on-demand shader / pipeline counters, frame timing, the FPS graph (0 to
  60, as in Tears of the Kingdom and Age of Calamity: `front_graph.h`) and the shortcut list; output size and controller layout leave the number keys (settings window only) and key 7 is gone.
  It starts hidden without a settings file; a file from an earlier build keeps what it showed, and is saved as hud= /
  hud_lines=. `DM_AOC_STATS` / `DM_AOC_FRAME_GRAPH` impose lines for one run and are never saved. Statistics are collected
  while a line that needs them shows. The shader lines count on-demand Metal library loads and pipeline build attempts
  (failed ones included) against the libraries and render + compute pipelines ready, and turn yellow for three seconds after
  either count rises (`shader_activity.h`): a hint where to look, not proof of a compile or of a hitch's cause. Errors keep
  them orange; the first sample, and the first after the line or the box was hidden, light nothing.
- `runtime/games/01006BB00C6F0000/pso_manifest.tsv` is new, not from nxnative: the warm-up seed the app ships in place
  of the empty template, made by `scripts/pso_seed.py` from three Mac recordings of this project. On 2026-11-10 the
  player list of a first launch on an emptied Metal cache (the 8 pipelines and 9 shaders that launch still built on demand,
  when the saved game loads) was merged after it, the current list first; the header names both with their SHA-256. It holds shader and pipeline keys only, no game
  data, and is not a shader-chain input, so `assets/expected-1.1.0.json` and the shader pins are unchanged. Packaging
  refuses a staged seed that differs from it (`scripts/package_la.py` verify_seed).
- Settings → Shader list exports the rows of the player's `pso_user.tsv` that the bundled seed lacks to a file they
  choose, to send by hand (new `runtime/host/pso_export.m` / `.h`; `front_settings.m`, `front_settings.h`, `front_mac.m`,
  the host `Makefile`, `scripts/test_settings.py`). One read of the list; a row cut short at its end is left out and named,
  any other bad row stops the export; the file is written atomically; nothing is sent or marked. `nvn_mtl.m`'s
  `pso_user_path` is no longer static so the export reads the same file the game writes (its only change).
  `scripts/pso_seed.py --shader-map` refuses keys a 1.1.0 shader map lacks.
- `runtime/host/hle_hid.c`: scripted input (`DM_KEYS`, `DM_CTL`) shares one press, which after letting go waits until the game is
  handed a player 1 sample without that step's buttons (1 s at most, logged): a release shorter than a frame merged two presses
  into one. The wait can delay steps due at a time. `DM_HID_LOG=2` also logs each read's time, sampling number, history entries
  and frame while a button is down or just let go. `runtime/host/tests/t_hid.c` gets the timing stubs it lacked (it did not
  link) and drives both scripts step by step; `tests/test_hid_input.py` builds and runs it.
- AoC's profile, timing and performance tests and diagnostics scripts are not copied.
- `runtime/host/hle_movie.c`: native-texture movie slots wait for the client's release (nxnative `8bfc3a60`,
  GitHub issue #700, with the held-slot tracking it builds on from `4e37a225`), and the trace gives the time
  of the picture copied into the texture; `runtime/host/tests/t_movie_slots.c` runs on the real file.

Shader maps, facts, pairs and libraries are generated from the player's own files in the external
workspace. The prebuilt shader translator's file set and SHA-256 values are unchanged
(`prebuilt/macos-arm64/SHA256SUMS`). LibHac 0.19.0 is a SHA-256-pinned local prerequisite of
`scripts/extract_la.py`.

See [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) for transitive licenses.
