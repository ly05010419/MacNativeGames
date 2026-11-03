# Age of Calamity validation — 2026-10-27

## Keyboard shortcut follow-up — 2026-10-27

The game-window number shortcuts now include 2 (Original 30 → 60 → 120),
4 (dynamic timing), 5 (stats Off/FPS-only/Full) and 7 (frame-time graph), alongside
1 (hints), 3 (output cap) and 6 (controller layout). The compact hints pane shows
all seven controls and their current state. The settings help and README also
list the full gameplay keyboard mapping. Gameplay has no keyboard right-stick
camera-direction binding; the mouse supplies touch input.

The event monitor and UI tests share the same shortcut dispatcher. Modifier,
window-focus and repeat filtering remains in the game-window monitor. Native UI
checks passed 66 assertions per language (English/Chinese), including target
cycling, action mappings, preserving all gameplay keys, current FPS labels,
minimum-size bounds and no overlap between shortcut/stat panes at 640×360.
The native host rebuilt successfully; no shader or rewritten-game change is
needed for this UI-only follow-up. The updated
`~/Applications/Hyrule Warriors Age of Calamity Controls.app` was published as a
separate app using an APFS clone of the verified Dynamic FPS bundle. Its host
was replaced with the rebuilt arm64 executable, source/output fingerprints
were updated, and nested/bundle ad-hoc signatures passed strict deep verification.
The original app and shared shader/rewrite resources were preserved. Signed host
SHA-256: `fedc6afcae9e2716b8e18cdc7159a99595a7d10da786368b399daff5094f9490`. No game was launched for this follow-up.

## Latest: dynamic FPS and stats pane — 2026-10-27

This section supersedes the older compiler/package status below. The corrected
shader chain `37275f7782d0` completed with 40,729 successful extracted keys and
65,621 real Metal libraries; the original signed app was packaged and tested
before this FPS work.

AoC now offers Original 30 (default), explicit 30/60/120 targets, measured dynamic
stepping and independent Off/FPS-only/full stats plus a 240-frame timing graph.
Settings are persisted outside the bundle. Game-present and actual displayed
FPS are separate; headless/stale display measurements show unavailable.
Shader inventory, resident libraries, render/compute pipelines, synchronous
load/build costs, imported prewarm objects and load/render-pipeline failures
are distinct counters. Prewarm imports do not establish draw use.

The native timing ABI is now **3**. Four original-instruction-asserted sites
coordinate the task cap, KIDS clock, scene/battle clock and KTGL device.
KIDS uses `(post_wait_tick - preceding_tick) / frequency`; its pre-wait `s1`
value is work-only and is deliberately rejected by the regression test.
Scene/battle updates measure their own native counter interval, independently
of task/presentation cadence, preserving speed when the render rate falls below
the selected cap. Elapsed steps are bounded to 1–100 ms; long loads/focus gaps
are not replayed. Task frame-object timescales are retained. Live target changes
also update the device's present interval, including return to original 30.

Native CNTFRQ_EL0 is used for the scene counter: this M5 reports **1 GHz**, while
`mach_absolute_time` retains a **24 MHz** ABI. A native probe checks the resulting
elapsed seconds against Mach time. Reusing Mach's timebase for CNTVCT would be
incorrect on this machine. The loader refuses old rewritten timing ABIs.

New checks: 34 AoC tests and 5 shader-tool tests passed, including 8 instruction
execution tests using Unicorn and a UBSan C telemetry/control/counter probe.
Native settings controls passed 26 checks per language (English/Chinese), and
the actual settings window and target selector were inspected through AppKit.
Metal validation probes passed: vertex buffers 43, geometry 127, integer vertex
inputs 14, texture-buffer fetch 37 checks; zero failures.
Private logs: `WORK/tests-dynamic-final.log`, `WORK/tests-gpu-dynamic.log`.

Earlier exploratory runs `dynamic-120-01` and `dynamic-120-02` are not final
acceptance. The first exposed work-only time being mistaken for elapsed time;
the later scene-clock correction is covered by the native/instruction tests.
Final native run `WORK/validation/dynamic-final-01` used host
`50b641810e2710f44e7ed992225ad20ca9f2607ead5c2b3688c5a4170248c66c`
and exited 0 after 180.13 seconds. It reached both opening movies, skipped them
through ordinary Plus input, and entered battle. Live targets changed
120 → 60 → Original 30 → 120. Steady battle windows advanced the scene clock
7.389 seconds in 7.389 wall seconds and 7.359 in 7.359. These are clock checks,
not proof that rendering sustains the selected target. Six pre-existing texture
mismatch diagnostics remained; no missing-shader/pipeline failure or native
crash was logged. Private summary: `validation/dynamic-final-01/validation-summary.json`.
Package copying began near the run's end; this run is not a performance benchmark.
The separate app `~/Applications/Hyrule Warriors Age of Calamity Dynamic FPS.app`
was packaged without replacing the earlier app. Its signed host is
`45d868519ee2d545baf9224081d9dae06af4079607095ecd03d797b69eb571fe`;
all source fingerprints matched the working tree at packaging. This validation
record was subsequently updated to record the following results.

`validation/dynamic-packaged-02` ran the actual signed launcher with a disposable
`DM_AOC_PROFILE` and 120/dynamic settings, exited 0 after 90.13 seconds, and
passed strict deep signature verification before and after play. A premature
`dynamic-packaged-01` test was attempted before atomic app publication and
failed its pre-launch existence/signature check; no game ran in that test.
The UI automation tool later timed out and launched a second instance with the
normal profile path. It was stopped; filesystem birth times established that
all of that profile was newly created by this launch. It was preserved as
`validation/dynamic-ui-automation-profile`, restoring the previously absent
normal profile. No game/test app remains running. This overlapping UI attempt
means the packaged session is a launch/signature smoke test, not a benchmark.
The actual stats/selector layout was verified separately in the native settings
harness. Full foreground display-cadence acceptance remains open.

60/120 remain experimental targets, not sustained frame-rate promises or full
physics/animation/movie/save acceptance. The existing six texture-type mismatch
diagnostics, gameplay-progress save/reload gate and long-play validation remain
open. Neither a compiled shader inventory nor zero load/PSO errors establishes
complete rendering correctness.


**Resumed at the user's request.** The first whole-game shader build completed
at 14:49 EDT: 40,729/40,729 keys reported success, with 65,621 real Metal
libraries including variants. Auditing found two unresolved geometry storage
bindings despite successful compilation. Both original shaders read c0 word
260 (byte 0x410), geometry storage slot 0. The offline reflection and runtime
driver-table mapping now recognize that stage. Packaging rejects unresolved
bindings, unknown binding kinds and unfillable driver reads. A new chain build
is in progress; unchanged MSL can reuse libraries through the compiler-keyed
Metal cache. No final app has been packaged. Progress save/reload still requires
a genuine game save point.

Resume source tests: 23 AoC tests and 5 shader-source tests, all passed.
Native Metal probes rerun with validation enabled: t_vbuf 43, t_geometry 127,
t_vin 14 and t_tbuf2d 37 checks, all zero failures. Logs:
`WORK/tests-resume.log`, `WORK/tests-gpu-resume.log`. Initial complete-cache run
`full-01` used host `21c5688c...`; its seed prewarmed 223 shaders, 290 render
pipelines and 28 compute pipelines in 738 ms with zero stale keys or pipeline
failures. It exited 0 after 226 seconds during Movie001, before natural
completion; this does not pass the final movie or gameplay gates. Two early
AudioUnit callback stalls recovered by restarting; subsequent logged callback
windows reported zero underruns. Final acceptance must use the updated host
and libraries. Private audit/evidence: `WORK/validation/shader-audit.json` and
`WORK/validation/geometry-evidence/`.

Implementation and tests are in progress. This record distinguishes new local
results from historical upstream/donor reports in ANALYSIS.md. **The native first
battle is running with visible combat, and the user confirms DualSense input and
game audio work. Whole-game shader completion, rendering completeness, gameplay
save/reload, final packaging and performance acceptance remain open.**

## Reproducibility

- Repository HEAD: `8c9c71b9503f9b7d9643a4f45aa8fbcb1aff1f02`, plus uncommitted
  AoC-only implementation. Original planning documents were already untracked.
- Foundation: nxnative `7861e7c1a9913e020600b1cc7f65132427e9cc50` through the pinned
  Jamboree copy. Copy and target profile manifests in `third_party/`.
- Hardware: Apple M5, arm64, 16 GiB RAM, 10 CPU cores; macOS 27.0.1 (26A434).
- External workspace: `/Users/jansantiago/MacNativeGames-work/aoc-1.3.0`.
  GPU probes: sibling `aoc-probes`; settings tests: sibling `aoc-settings`.
- Xcode-selected SDK/Metal compiler: macOS 27.0, Metal 32023.921. .NET SDK
  10.0.403 installed privately under WORK/dependencies/dotnet from Microsoft's
  release metadata, with the archive's published SHA-512 verified; metadata is
  recorded in `dependencies/dotnet-source.json`. Homebrew spirv-cross 1.4.363.0.
- Python: existing analysis venv `/private/tmp/macnative-aoc-analysis-venv`.
  Core packages: lz4 4.4.5, capstone 5.0.9, fonttools 4.68.0, Python 3.14.7.
  Xcode 27.0 (27A266a). Final source fingerprints accompany the packaged build.

## Input verification — passed for extracted-file scope

Supplied root: `/Users/jansantiago/Downloads/RYUJINX/GameRoms/_Files AOC`.
No original file was changed. The key file was neither read nor copied: these
steps need no keys, containers or LibHac. Separate APFS clones are in
`WORK/build/extract/v327680`.

- ExeFS contains original NSO `rtld`, `main`, `subsdk0`, `sdk` and `main.npdm`.
  All three runtime-module build IDs match the pinned 1.3.0/v327680 profile.
  Full file/segment hashes and sizes are in `assets/expected-1.3.0.json`.
- NPDM META and ACI0 ranges checked; program ID `01002B00111A2000`, system
  resource **0**, main priority 44, core 0, stack 1 MiB. Profile explicitly sets
  `sys_resource=0`. NPDM signature verification was not performed.
- RomFS is a **directory tree**, 1,101 files, 11,719,163,910 bytes excluding
  Finder metadata. Canonical inventory SHA-256:
  `99a66f924dfd38da7c10bebf2afb25d9a5607a867e8c6ff66bdb2b64774b4cf3`.
  Every file's SHA-256 is private in `WORK/input-report.json`.
- NSO mapped extents total 66,453,504 bytes before rewritten code/host stacks.
  The imported `mem_used=0x4800000` is retained. A diagnostic run successfully
  allocated the game's 3.14 GiB heap without enlarging app_pool.
- No original containers or content metadata were supplied. Container/NCA
  integrity, signatures and base/update composition **cannot be claimed**.
  Version identity comes from matching module builds and pinned extracted bytes.

## Native rewrite and startup — passed; rendering not implied

All three modules rewrote successfully (first pass 24 seconds). Main applied
five build-gated, original-instruction-asserted patches: two sound subheap
adjustments and three frame-step branches. SDK/subsdk0 applied no fixed-address
cuts. Main rewrite counts: 250 windows, 35,413 plain sites, 1,043 conditional,
519 PC-relative, 3 indirect branch forms, 1,089 spin regions, 3 frame-step sites.
Full counts and declared patches are in `build/logs/rewrite_*.log` and
`build/patched_v327680/*.cuts.tsv`. The final rewrite also asserts all 12
displaced bytes at the KIDS step site and records/verifies 19 output hashes.
Repeating the rewrite with these checks produced byte-identical module images.

The sound patches move 4 MiB from the 142 MiB data heap to the 15 MiB engine
heap; total is unchanged. Their underlying hardware/work-buffer discrepancy
remains unresolved. They are compatibility workarounds.

Original simulation default: `fps_target=0`. The off stubs preserve task/battle
step values but still set device frame rate to 59.94 / game-derived vsync count.
This is not an absence of generated frame code. 60 fps timing is unvalidated.

`validation/startup-01`: bounded 20-second headless run; host SHA-256
`36c634026e5ea319b62a2bf0551f00d689961b29f638eb8585e8e7ad83774242`.
Reached nnosInitialize, nninitInitializeSdkModule, nninitStartup, module DT_INIT,
original nnMain, RomFS mount, SDK CPU audio renderer and repeated presentation.
SDK detected `nnSdk-9_6_0-Releas`. Undefined symbols observed at load were weak;
`stubs=0` at startup. SIGTERM drained graphics/audio and exited 0. Shader
libraries were incomplete and draws reported missing libraries: **this run is
not picture, audio-output or performance acceptance**. Compile work overlapped.

## Offline shaders — in progress

Fresh extraction from originals: 40,729 unique keys: 241 compute, 2 geometry,
37,045 pixel, 3,441 vertex; 40,612 deduplicated keys selected from system resources
and 117 from main. 241 compute-fact rows and 3,441 vertex/pixel pairs. This is an
inventory, not proof of complete extraction or correct rendering. Log:
`build/logs/shader_split.log`. Public-format tables are generated privately.

Chain `3d0c91995e05` uses the unchanged pinned translator, actual local compiler
identity in per-key/variant fingerprints and `-fpreserve-invariance` offline
Metal compilation. Full build is running into `build/shader/cache`; it has not
completed. Source and launcher do not force runtime MSL. Normal packaging
rejects placeholders, missing keys and mismatched input stamps.

Separate diagnostic cache `validation/shader-cache`: the 14 keys requested
during initial startup compiled offline, 14/14 success, no undecoded binding
slots or unrecognized binding kinds. This partial cache has no full-build stamp;
uniform proof tables correctly remain disabled. `title-01` produced a legible
first-run difficulty/voice/text settings screen with visible background and UI;
four additional requested shader keys were missing as it advanced. It is not
complete title/movie rendering acceptance. The full build and diagnostics
overlap, so these windows are not benchmarks.

## Interactive first battle — reached, completeness still under test

`movie-03`: the opening VP9 movie rendered upright with visible colors and game
audio. Its decoder completed 5,589 input/output packets (5,588 decoded pictures),
consistent with the original Movie001.webm duration 186.452937 seconds. The next
movie and initial battle were reached. The shorter second decoder was stopped
after 176 pictures; that is not a natural-completion claim for that movie.

`battle-01`: a fresh process used a copy of movie-03's committed 1 MiB `svdt`
save, initially SHA-256
`9e9a16102b28fa0bdf12a31872e8fb3d489adae6ded6b407c63ba84ff29995fb`.
The first battle renders Link, animated Bokoblins, terrain, grass, lighting and
HUD, with at least 64 KOs observed. No progression or character state was forced.
231 observed/dependency keys compiled offline (231/231), including 35 shadow-input
libraries (0 compile failures) and 38 fetch variants. Sixteen newly reached
effect keys then compiled 16/16. Reached-scene completeness is still being checked;
earlier missing-shader windows do not count as correct rendering.

The physical DualSense Wireless Controller was detected as player 1, with
buttons mapped by PlayStation positions. Native HID logs show physical button
and stick changes during combat. **User-confirmed in this session:** controller
movement, attacks, pause and game audio work; keyboard WASD movement, I attack
and Return pause/resume work. These are human confirmations plus native logs,
not claims from synthetic tests. CoreHaptics reports helper/XPC error -4810 for
rumble; rumble correctness remains open. It does not prevent reported input.

An audio sizing diagnostic attempted the original hardware sizing path, which
connected to unsupported `audren:u` and exited 42 in `movie-02`. That diagnostic
was removed and the host rebuilt. The working CPU/manual path remains intact;
the hardware sizing discrepancy is unresolved. Current host SHA-256:
`fe6c32d28440615f1e1208303042a0a528e4878727a53961c3735396f4fd5431`.

All these runs overlap whole-game shader compilation and use a partial cache
without full-build uniform proof tables. They establish bring-up/input progress,
not final picture completeness or performance. Final save/reload must verify
game progress after a game-initiated commit, not merely an initialized save file.

## Newly run source and native tests

| Check | Exact result | Scope |
|---|---|---|
| AoC packaging/profile/input/timing tests | 22 tests, all passed | Synthetic negative controls include wrong build/NPDM, corrupted NSO segments, changed tree bytes, unexpected translator files, stale shader stamp, placeholders, folded-geometry metadata, atomic existing-app preservation, changed rewrite outputs/rules, original patch bytes and delayed GPU timing rows |
| Copied shader source tests | 4 tests, all passed | Four-column maps and rasterizer-discard writes |
| t_vbuf, Metal validation | 43 checks, 0 failed | Synthetic buffer behavior |
| t_geometry, Metal validation | 127 checks, 0 failed | Synthetic geometry/indirect/discard behavior |
| t_vin, Metal validation | 14 checks, 0 failed | Mixed integer vertex fetch |
| t_tbuf2d, Metal validation | 37 checks, 0 failed | Wrapped texture-buffer fetch |
| Copied host tests, `make ... run` | Exit 0 | Files/save transactions, threading, audio, HID, font, time, lifecycle and NRO synthetic probes |
| Native settings UI | 14 checks per language, 0 failed, English and Chinese | Controls and output sizing; no game |
| Launcher clang arm64 with -Wall -Wextra -Werror | Exit 0 | Build only |
| Root Python suite | 460 tests, failures=8, skipped=1 | Same existing settings-panel subcases: 1511x850 window, expected height 851; unrelated donor/root files unchanged |

Logs are `WORK/tests-{build,tools,gpu,host,settings,root}.log`. No game screenshots,
PCM, saves, translated shaders or libraries are committed.

## Remaining acceptance gates

Finish whole-game offline libraries and classify all translation/compile/parse
failures; regenerate stamped c1/cbuffer tables; rerun final rewrite and native
host; test title/movie natural completion and skip, active battle soldiers,
skinning, lighting, decals/effects, keyboard and a physical controller, audible
continuous music/movie/game audio, actual save and full-process reload. Then
package and verify the self-contained signed app before and after play, and
measure fixed-route cold/warm startup and battle frame distributions without
compilation/diagnostic confounds. No performance or gameplay promise is made.
