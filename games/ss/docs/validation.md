# Skyward Sword HD 1.0.1: validation records

What was measured on a Mac for this target, how, and what is still open. Game files, saves and logs stay with the
person who ran them; this file keeps only the method and the numbers. Mac: Apple M5 Pro, macOS 26.5, Xcode 26.6
(Metal 32023.883), 2026-11-11.

## Inputs

- The update's four modules have nxnative's build IDs: `main` 5441af3c…, `subsdk0` 4dd097b4…, `subsdk1` 535249a5…,
  `sdk` b39add63….
- `scripts/extract_ss.py` on the base game and update 1.0.1: pass, 882 files, every header signature valid. The base
  game alone: pass, 882 files.
- `scripts/compose_check.py`: the update changes 3 RomFS files of the base game's; the shader split of the merged
  folder gives 2,470 keys whose stage / key digest equals nxnative's map (`b16cfddb…`).

## Tests

- `make test`: `tests/` 65 OK (2 skipped without `SS_TEST_MAIN`; with the extracted 1.0.1 `main`,
  `tests/test_isolation.py` 3 / 3, its control included); `runtime/tools/tests` 34 OK.
- `make test-gpu`: t_vbuf 115, t_geometry 1,920, t_vin 14 and t_tbuf2d 37 checks, none failed.
- nxnative's graphics tests (`runtime/gfx/tests`, `make GAME=01002DA013484000 run`, 111 programs): none failed; the
  shader budget test needs the profile's shader map, which only a build makes: it passes in a build's `source/`.
- nxnative's host tests (`runtime/host/tests`, `make run`): none failed.
- `make test-ui`: 112 checks, none failed.

Found by these tests and fixed: Age of Calamity's display-link hold for every frame period broke the present
allocators of this runtime (`t_present_alloc`: 6 of 13 failed; with nxnative's 25 ms threshold 13 of 13, as nxnative
`b1dee89` itself with only the macOS 27 SDK guard on this Mac); the shader list export expected 432-byte pipeline
keys where this runtime records 440; the rewriter loaded rules from outside profile folders (`DM_GAMES_DIR`).

## Build

A new work folder, no cache: 2,470 of 2,470 shaders `ok` (412 vertex, 2,058 pixel), 7,122 Metal libraries (2,470
base, 2,048 logic-operation, 2,048 alpha-test), 432 dual-source entry points (the 432 keys of `dual_want.tsv`).

The host in the app differs in SHA-256 from the one the build leaves in `build/tools`: packaging signs it. With both
signatures removed, `__DATA` and `__DATA_CONST` are equal and `__TEXT` differs in 6 bytes, all in the Mach-O header and
load commands (the `__LINKEDIT` size and the signature command).

## Runs

Same route twice with the build's host, scripted input (`DM_KBD=Link`): the opening movie, a new file named Link,
the opening cutscenes; 135 s each, 7,416 frames.

| Run | Exit | Red lines | FPS after frame 600 | Frames over 25 ms | Over 50 ms | Pipelines built on demand |
|---|---|---|---|---|---|---|
| 1 | 0, graceful | 0 | 59.2 | 65 | 19 | 137, 2,038 ms |
| 2 | 0, graceful | 0 | 59.2 | 65 | 19 | 137, 1,989 ms |

The fog's logic-operation library loaded in both. From the packaged app (its own launcher, a disposable profile):
signature verified, the file list reached, exit 0 after SIGTERM with a graceful exit, no red line. The corner box and
its statistics were checked by hand in the app.

An earlier exploratory run with another tree and host (Age of Calamity's display-link hold) had 32 frames over 25 ms
and 20 over 50 ms on the same route; the difference in frames over 25 ms is not explained.

## Open

- A play-through past the opening; combat, flight, the dungeons; a game controller in a long session.
- The warm-up list: the opening route recorded 153 pipeline rows of 880 hex digits, not yet merged or measured.
