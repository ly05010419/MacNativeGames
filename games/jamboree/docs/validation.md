# Validation record

Date: **2026-10-25**. Game: **Super Mario Party Jamboree 2.3.0**, `0100965017338000`, `v458752`. Input NSO sizes, SHA-256 values and build IDs are recorded in [expected-2.3.0.json](../assets/expected-2.3.0.json).

Hardware: Apple M6 Mac mini (`Mac18,5`), Apple silicon / unified memory. OS: macOS **27.0.1 (26A434)**. This is the local test machine, not a performance promise for other Macs. Deployment target is macOS 26; macOS 26 itself has not been tested. iOS/iPadOS/visionOS were neither built nor played for this target.

## Initial bring-up checks (2026-10-25)

| Check | Result | What it establishes |
|---|---|---|
| Root `python -m unittest discover -s tests` | 331 passed | Existing repository source suite remains green |
| Game build tests | 8 passed | Original-input mismatch rejection, unsupported-title rejection, output preservation, source staging, external caches, compiler checksums |
| Synthetic discard translator tests | 2 passed | Void entry keeps side effects and early returns; correct storage write flag |
| Isolated native `t_vbuf` with Metal validation | 43 checks, 0 failed | Vertex buffer binding/invalidation behavior |
| Isolated native `t_geometry` with Metal validation | 127 checks, 0 failed | Geometry, transform feedback, discard-only vertex stores and instancing |

The GPU tests were rebuilt from **this game directory's copied source**, staged outside the checkout. No other game target supplied source, headers or compiler binaries. That initial build used translator chain `e6e2d522054e` and shader inputs `37da065929be`. The review-cleanup build below supersedes those identifiers; these dated results are retained as historical evidence.

## Private build of this contribution

The isolated target was built against local original base XCI + 2.3.0 update NSP. Previously extracted original files were reused and their pinned NSO hashes checked; **all three NSO modules and 139 NROs were rewritten again** by the copied rewriter. The shader split was recreated from those private originals (25,693 shaders), matching shader-cache entries were reused, and both runtime binding tables were regenerated. No game-derived outputs enter the source tree.

The final arm64 host and launcher were built with the macOS 26 deployment target. The final app passed deep strict signature verification, and its build-info host hash was checked against the **signed** nested executable. The bundled host then ran for a 60-second headless smoke test using a private save copy: it entered `nnMain`, loaded shader inputs `37da065929be` and 24,814 uniform-read extent records, and reached frame 2,100 without exiting. This verifies startup and rendering submission from the packaged paths; it does not establish visual correctness for additional scenes or physical input behavior. A separate pre-packaging host smoke run lasted 90 seconds.

## Native play observations before contribution packaging

The same geometry-fix source was compiled into the local native app and exercised on the above Mac:

- Opening/title flow and character selection reached the plaza.
- Character selection switched Mario → Donkey Kong → Mario → Peach → Mario. The large stretched triangles were absent in the inspected character views.
- Plaza welcome close-up/dialog and wide view rendered the character meshes without the earlier stretching.
- The producer ran with rasterizer discard enabled and transform feedback off. A translated morph-shader check matched 24/24 output values and bounds. That shader-derived test is private and is not redistributed.
- A 300-frame plaza window had frame-time p50 **16.66 ms**, p95 **17.24 ms**; GPU p50 **4.03 ms**, p95 **4.97 ms**. This is a local steady-scene observation, not a full-game benchmark.
- Saved face-button layout was read as `label`. Physical controller presses were not exercised.
- The local packaged app passed deep strict signature verification both before and after launch.

Screenshots, translated shaders and saves used for local verification are not included in Git because they contain or derive from game content.

## Known limitations

- Full shader processing yielded **25,690 / 25,693** successful translations. Three existing failures report `Subgroup builtins are not available in this type of function` from SPIRV-Cross. Their scene coverage is unknown; they remain unresolved.
- One shader still reports an unrecognized binding kind. Many translation queries use defaults because this game's fact table does not answer them. Reaching the tested plaza does not establish correctness in other scenes.
- Some first-visit scenes paused or briefly showed white for **9–17 seconds**, then resumed rendering. Runtime shader compilation remains visible to the player; no claim of smooth first traversal is made.
- Full boards, all minigames, motion input, multiple physical controllers, long sessions, memory stability and save round trips have not been comprehensively verified.
- Only the pinned update is accepted. Other updates, DLC, mods, online features and compressed containers have not been validated.
- The app uses local ad-hoc signing. Distribution/notarization and operation on another Mac have not been tested.

## Reproduce without game content

From the repository root:

```sh
python3 -m unittest discover -s tests
make -C games/jamboree test
make -C games/jamboree test-gpu WORK=/path/to/private/Jamboree-gpu-tests
```

For the private full build, use the command in [README.md](../README.md). Logs stay under `<work>/input/0100965017338000_build/logs/`. Preserve the generated app's `build-info.json` and the existing `patched_v458752/build_src.sha256` when reporting an issue. Share a redacted log and version identifiers; do not upload game programs, shader source/libraries, keys or saves.

## macOS settings follow-up (2026-10-25)

The Mac-only front end adds its own dark settings panel, a compact shortcut HUD and output-size caps. No shader chain, guest instruction patch or internal render resolution changed. The original shader failures and untested gameplay modes above remain applicable.

- Root source suite: 331 passed; existing game/translator tests: 10 passed.
- Native AppKit settings harness: 14 checks in each of Chinese and English (28 total), zero failures. Covers actual popup/checkbox actions, unavailable MetalFX state, reopening, screen bounds, automatic/native sizing, a 4K-to-1080p cap, non-16:9 aspect preservation, smaller windows and invalid/zero dimensions.
- Rebuilt the arm64 2.3.0 host from this target and exercised it in an isolated app with a private save/config copy. Cmd-comma and F1 opened settings; the output selector changed the drawable from 2560×1440 to 1920×1080; A/B swap, hint toggle, output and layout hotkeys updated the real front-end state and persisted file.
- Fullscreen worked with the settings panel open. A second launch restored the selected output cap, A/B swap, hidden hints and startup-fullscreen preference. The title scene continued submitting frames during the test; this does not add coverage for other game scenes or physical controllers.
- The installed local app is updated only after backing up its executable/signature metadata; signature validation is repeated after the update. UI test screenshots and private runtime data are not committed.

Run `make -C games/jamboree test-ui WORK=/path/to/private/Jamboree-ui-tests` in a macOS GUI session. The harness contains no game content, keys or game-derived shaders.

## Maintainer review cleanup (2026-10-26)

The translator is now **binary-only**, with 12 required files (10 binary payloads and two JSON metadata files). Its exact file set and full SHA-256 sums are checked before staging. Duplicate C# translator/Ryujinx source trees, all PDB files and Humanizer satellite resources are removed. Required licenses and pinned upstream source provenance remain. The native host still compiles libvpx from source for movie playback; no libvpx binary is distributed.

The public shader map contains only generated package/index/stage/key. Both its writer and translator/diagnostic readers use this four-column format; the translator rejects an empty map or the old path-bearing format instead of silently building zero shaders. The profile uses generic grouping and an atomic extraction-root marker, with no original asset paths.

- After integrating upstream main `33e8fa8`, root suite: **345 passed** (331 passed before integration).
- Target suite: **12 packaging/input/tooling tests + 4 translator/map tests**, all passed. This includes reading all 25,693 public shader keys, rejecting old/empty maps, rejecting missing/modified local LibHac packages, rejecting unlisted translator files, and removing obsolete tooling from an existing workspace.
- Metal regressions rebuilt after distribution cleanup: **43 vertex-buffer + 127 geometry/discard/feedback checks**, zero failures, Metal validation enabled. These use synthetic shaders; the subsequent map-reader fix changes Python build tooling only.
- Distribution audit: all binary payloads are confined to the checksummed translator; no dependency package, PDB, satellite DLL, duplicate translator source, game content or original resource-path column remains. The PR diff against upstream changes only `games/jamboree/` and one root README line. Local documentation links and retained provenance destinations were checked.

The LibHac 0.19.0 package is an explicit local prerequisite, pinned to SHA-256 `1431267861db5ff68216315b164f10c0edab4df162cedf5429bf8a143b76de19`; it is not committed or downloaded by the builder. Verification starts with an **empty** work-local NuGet cache and wraps the builder and all children in:

```sh
sandbox-exec -p '(version 1)(allow default)(deny network*)' \
  python3 games/jamboree/scripts/build_jamboree.py /private/inputs \
  --keys /private/prod.keys \
  --libhac-package /local/dependencies/libhac.0.19.0.nupkg \
  --work-dir /private/build \
  --output '/private/Jamboree Offline.app' --language zh-Hans
```

The restore record has one package (`LibHac/0.19.0`) and one source (the work-local filesystem feed). NuGet's configuration clears inherited feeds/fallbacks; restore disables auditing and compile uses `--no-restore`. The generated app contains neither LibHac nor .NET build tools. A locally cached original extraction and fingerprint-matching rewritten modules are reused; this is a cold NuGet/offline validation, not a claim that every private intermediate was rebuilt from an empty work folder.

The network-denied build completed successfully. The shader split was regenerated and all **25,693** four-column identifiers matched the public profile. The full shader pass reported **0 cache hits, 25,693 rebuilt, 25,690 successful translations**, plus the same three SPIRV-Cross subgroup-function failures; the known unknown-binding warning remains. It regenerated 5,237 shadow-input libraries, `c1_full.tsv` (5,313 shader keys) and `cb_extent.tsv` (24,814 shaders), rebuilt the arm64 native host, packaged the app and passed deep strict signature verification.

Current chain: **`285c1c5ec4a2`**. Shader inputs: **`cac33153e188`**. Both generated runtime tables and the packaged library stamp agree. The signed host SHA-256 recorded in `build-info.json` was verified as `6ee3c808ed6d8cac189105280844686d2668733c506e6189c11afb38dc4e3d08`. As before, these outputs use runtime Metal compilation of translated MSL; this is not a claim that GPU pipelines are all compiled before play or that scene-transition pauses are fixed.

The packaged host then completed a **60-second headless smoke run**, entered native `nnMain`, loaded the matching library stamp and 24,814 uniform-read extent records, and reached **frame 1,800** while remaining alive. It used an existing private test-save copy and separate temporary/config directories. Deep strict app-signature verification passed again after the run. This confirms packaged startup and rendering submission, not additional visual/gameplay or controller coverage; existing first-use Metal compilation stalls remain visible in timing logs.
