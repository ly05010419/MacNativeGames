# Skyward Sword HD 1.0.1: validation records

What was measured on a Mac for this target, how, and what is still open. Game files, saves and logs stay with the
person who ran them; this file keeps only the method and the numbers. Mac: Apple M5 Pro. 2026-11-11: macOS 26.5, Xcode
26.6 (Metal 32023.883), the first tests, build and runs. 2026-11-13: macOS 27.2.1, Xcode 27.2 (Metal 32023.922, the one
`dmsh_chain.lock` records), the review changes: the translator built from source, the libraries from `tools/deps`, the
strict compiler check, the warm-up list and the packaged app.

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

2026-11-13, on the reviewed tree: `tests/` 69 OK (2 skipped without `SS_TEST_MAIN`), `runtime/tools/tests` 34 OK, the
repository's `tests/test_no_homebrew.py` OK, and the host's video tests (`t_movie_vpx`, `t_movie_avc`, `t_codec_video`)
linked against `tools/deps`' libvpx 1.16.0 and dav1d 1.5.3: all pass. `make test-gpu`, `make test-ui` and the graphics
battery were not run again.

## Build

A new work folder, no cache: 2,470 of 2,470 shaders `ok` (412 vertex, 2,058 pixel), 7,122 Metal libraries (2,470
base, 2,048 logic-operation, 2,048 alpha-test), 432 dual-source entry points (the 432 keys of `dual_want.tsv`).

The host in the app differs in SHA-256 from the one the build leaves in `build/tools`: packaging signs it. With both
signatures removed, `__DATA` and `__DATA_CONST` are equal and `__TEXT` differs in 6 bytes, all in the Mach-O header and
load commands (the `__LINKEDIT` size and the signature command).

2026-11-13, the reviewed tree, a new work folder: the translator built from `third_party/nxnative` with `tools/deps`'
SDK 10.0.401 (dlls as `dmsh_chain.lock` records them), SPIRV-Cross `f11ba9f`, Metal 32023.922 with the strict check
(no other compiler allowed): chain `814ff6e91b75`, 2,470 of 2,470 shaders `ok`, none from a cache, in 1:41. Every
shader's MSL is byte-identical to the 2026-11-11 build's (the prebuilt translator, the carried SPIRV-Cross): the
translator built from source gives the same SPIR-V and reflection on all 2,605 translator jobs of the build, and the
two SPIRV-Cross builds the same MSL. Only the Metal libraries differ (another compiler). Built host `f70b7f9d…`; in the
app `d26751ed…`, which differs only by its signature (`__LINKEDIT` size).

## Runs

2026-11-11, same route twice with the build's host, scripted input (`DM_KBD=Link`): the opening movie, a new file named Link,
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

## Warm-up list (2026-11-13)

Build: chain 814ff6e91b75 (translator built from `third_party/nxnative`, MacNativeGames' `tools/deps`, Metal 32023.922 / Xcode 27.2,
macOS 27.2.1), host sha256 `f70b7f9d…`, 2,470 of 2,470 shaders. Every run: the same build, a new disposable profile, the opening route
(`scripts/run_ss.py --window --audio --seconds 135`, `DM_KBD=Link`, the keys of the 2026-11-11 runs), screenshots at 65 s and 125 s.
Pipelines built on demand: the host's last `[bootprof]` line, "pipelines created N / ms (all synchronous on the submit thread)".

| Run | `DM_PSO_MANIFEST` | Built on demand | Put in the pipeline cache by the warm-up | Prepare before the first frame | Exit, RED |
|---|---|---|---|---|---|
| rec1 | off (recording) | 137 / 2,121 ms | 0 | none | 0, 0 |
| rec2 | off (recording) | 137 / 2,397 ms | 0 | none | 0, 0 |
| ctl1 | an empty list | 137 / 2,369 ms | 0 | none | 0, 0 |
| cand1 | the candidate list (explicit path) | **0** | 137 render pipelines | 0.33 s (125 bases compiled, 12 reused) | 0, 0 |
| ctl2 | an empty list | 131 / 2,369 ms | 0 | none | 0, 0 |
| cand2 | the candidate list (explicit path) | **0** | 137 render pipelines | 0.35 s | 0, 0 |

The two recordings gave the same 137 pipeline rows in the same order, also the set of the 2026-11-11 runs; the list is their merge
(`pso_seed.py --shader-map … --kinds RC`, 142 lines). Control and candidate alternated, after the recordings, so macOS's Metal cache
already held these pipelines in every A/B run (its state is not part of the result). Frame times are not the result either: in
cand1 from about 80 s and in ctl2 from about 115 s the display link fired at about 28.6 Hz (frame time p50 35 ms with 0.7-0.9 ms
of submit work, the host's power line 28.5 fps), the display's rate rather than the game; cand2 held 60.0 fps with no frame over
50 ms outside the screenshots.

From the packaged app with the list bundled (repackaged from the same work folder: 2,470 libraries from the cache, the
app's `pso_manifest.tsv` byte-identical to the tested list, host `d26751ed…`), its own launcher, a new disposable
profile and no `DM_PSO_MANIFEST`, the same route: the host read the app's list (137 lines), prepared it in 0.33 s before
the first frame, putting its 137 pipelines in the cache, and built 0 pipelines on demand; 60.0 fps, exit 0 with a graceful
exit, no red line. This run, not the A/B (which named the candidate list explicitly), checks the list bundled by default.
The host's `prewarmed and used` counter counts pipelines put in the cache (`nvn_mtl.m` `warm_claim`), not draws that used
them; the result is the count of synchronous builds.

## Open

- A play-through past the opening; combat, flight, the dungeons; a game controller in a long session.
- The warm-up list covers the opening route only; places past it still build their pipelines on first use.

## MNG integration review (2026-11-14)

The results above are the contributor's M5 Pro game runs. The integration checks below
ran on an M4 Mac with Xcode's Metal 32023.883 and iPhoneOS26.5.sdk. No Skyward Sword game
image was available on this machine. The complete game build still requires the locked
Metal 32023.922; its check was not bypassed.

- The full Mac host compiled and linked. The modified shared Metal backend also compiled
  for iPhone; this is a compile check, not an iOS app build or device run.
- The translator built from source. All three DLL identities matched `dmsh_chain.lock`.
  This local check did not replay the game's 2,605 translator jobs.
- A synthetic shader/PSO fixture passed four fresh-process checks: create an archive,
  reuse it without compiling, load a read-only bundled seed, and reject a bad manifest
  row. The production `mac_pipelines` wrapper ran twice against the fixture, including
  its fresh-process archive verification; the second run preserved the archive bytes.
  The existing PSO warm-up probe passed 98 checks and shader-map warm-up passed six.
- Settings checks ran twice: 112 checks each, none failed. Runtime tool tests: 34 passed.
  Target Python tests: 76 tests, none failed, two skipped without `SS_TEST_MAIN`.
  Signing/publication tests preserved the App, shader bundle and game-data bundle
  inodes, and restored the previous content when final signature verification failed.
- The root suite ran 1,514 tests: nine failures, two skipped. Failures concerned existing
  install-progress assertions, storage-path quoting, a mod-library preference, four
  Breath of the Wild settings cases, and a startup-memory test fixture. The five
  non-settings failures reproduced with the pre-merge `build_app.py`; the settings
  probe and its production dependencies were unchanged by this PR. These failures
  remain open and are not counted as passing integration checks.

Packaging review:

| Requirement | Result and evidence |
|---|---|
| Separately signed shader data | Implemented: `ss_bundle.signed_bundle`, real signing/reuse test. |
| Parallel archive creation during packaging | Implemented: `ss_pipelines.mac_pipelines`, manifest probe and fresh-process replay. |
| Finish warm-up before the title | Implemented: launcher forces `DM_PSO_PREPARE=1`; existing runtime waits for preparation. |
| Run build tools outside the App | Implemented: host and launcher build in `build/tools`. |
| Reuse verified hashes and signatures | Implemented: per-file identity cache and signed bundle reuse. |
| Keep the installed App directory | Implemented: common `build_app.install_app`, including rollback. |
| Runtime-only express updates | Implemented: MNG/update planner and `ss_update`; real-game execution remains unverified locally. |
| Retain matching translations and archives | Implemented: translation chain/input checks and per-descriptor archive lookup; runtime changes do not clear either cache. |
| Reuse verified installed game data | Implemented: `adopt_inputs` checks expected hashes; unchanged signed data is retained at publication. |
| Common dependencies without Homebrew | Implemented: `tools/deps` provides the native dependencies and LibHac; Python requirements use the work environment. |
| Lists beside the shader map in `build/` | Implemented: package and launcher use that directory; archive/list digests must match. |
| MNG entry point and common work directory | Implemented: `games/ss/build.py`, builder routing, recorded/default work folder and update planning. |
| Grouped iOS archives and retained iOS build cache | Not applicable yet: this target has no supported iOS packaging entry point. |
| Website contribution intake | Not implemented: SS uses 16-digit shader keys and 440-byte recipes; the shared collector expects another format. Keep this experimental target's export local until the website, uploader and collector support its format and game ID together. |

No local test above establishes gameplay correctness, frame rate, controller support
or stability beyond the contributor's recorded opening route.
