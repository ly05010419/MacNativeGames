# Hyrule Warriors: Age of Calamity — experimental native macOS target

[Source copies](third_party/COPIES.md) · [Licenses](THIRD_PARTY_NOTICES.md)

This directory contains an isolated Apple silicon target for **Hyrule Warriors:
Age of Calamity 1.3.0**, title ID `01002B00111A2000`. The original ARM64 modules
are rewritten ahead of time and execute natively; Horizon services run through
the host and NVN commands map to Metal. This is an experimental bring-up. Native
gameplay has been reached with keyboard and DualSense input and audio. Full
rendering, save/reload and packaged-app acceptance remain in progress; a
successful build is not a claim that the full game works.

The supported input is an unmodified, already merged **1.3.0 / v327680 extracted
directory**, containing `ExeFS/{rtld,main,subsdk0,sdk,main.npdm}` and a `RomFS/`
directory tree. NSP/XCI files, keys and LibHac are not needed for this mode.
Other builds and RomFS images are rejected. This path does not claim base/update
composition, NCA signature verification or container integrity verification.

The inspected input's NSO hashes/build IDs, segment hashes, NPDM and RomFS tree
digest are pinned in [expected-1.3.0.json](assets/expected-1.3.0.json). Every
build verifies the files, uses APFS clones in a separate workspace, and never
patches the originals. The tree digest is SHA-256 over sorted UTF-8 rows
`sha256<TAB>size<TAB>relative POSIX path<NEWLINE>`; Finder `.DS_Store` is
excluded. The full per-file inventory is local build data and is not included.

## Build

Use an Apple silicon Mac with macOS 26 or later, Xcode and its Metal toolchain.
Build dependencies are Python 3 with `lz4`, `capstone`, `fonttools`, and
`backports.zstd` when using Python before 3.14; .NET SDK/runtime 10; `spirv-cross`,
`zstd`, and `make`. The translator payload is pinned and verified before
staging. No build tooling is bundled in the runtime app.

```sh
python3 -m venv /path/to/aoc-python
/path/to/aoc-python/bin/pip install lz4 capstone fonttools backports.zstd
/path/to/aoc-python/bin/python games/aoc/scripts/build_aoc.py "/path/to/extracted" \
  --work-dir "/path/to/private-aoc-work" \
  --output "/path/to/Hyrule Warriors Age of Calamity.app"
```

Work and output must be outside the checkout and original input directory. Use a
new output path; an existing app is preserved. The builder supports `--phase
inspect`, `rewrite`, `host`, `shaders`, or `package`; the default `all` runs all
stages. Packaging publishes a temporary bundle only after signing and strict
verification. The app stores save data, preferences, temporary files and logs
under `~/Library/Application Support/Hyrule Warriors Age of Calamity/`.

## Controls and diagnostics

Keyboard gameplay mappings:

| Key | Controller input |
|---|---|
| WASD | Left stick / movement |
| Arrow keys | D-pad |
| J / K / U / I | A / B / X / Y |
| Q / E | L / R |
| Z / C | ZL / ZR |
| F / G | Left / right stick click |
| Return / Tab | Plus / Minus |
| Space | Shake |
| Left mouse click/drag | Touch screen |

The keyboard has no right-stick/camera-direction binding. Supported physical
controllers use GameController.

With the game window focused, these shortcuts apply immediately and save their
settings. Auto-repeat does not repeatedly toggle a setting.

| Key | Action |
|---|---|
| F1 or Command-comma | Open settings |
| 1 | Show/hide shortcut hints |
| 2 | Cycle FPS target: Original 30 ↔ 60 |
| 3 | Cycle output cap: Auto → 720p → 1080p → 1440p → 2160p |
| 4 | Toggle dynamic timing (inactive in Original mode) |
| 5 | Cycle stats: Off → FPS only → Full |
| 6 | Cycle physical-controller layout: Auto → Position → Nintendo labels |
| 7 | Toggle frame-time graph (visible when stats are enabled) |
| Control-Command-F | Toggle full screen |
| Command-Q | Quit |

The small shortcuts pane shows the current FPS target, dynamic mode, stats mode
and graph state alongside the existing output and controller controls.

For bounded diagnostics, `scripts/run_aoc.py --work-dir WORK --session NAME
--seconds 60 --window --audio` creates a disposable save and freezes the
executable/rewrite outputs. `--save-from` copies a prior validation save for
fresh-process reload checks. The session's `control.txt` accepts the host's
button and screenshot commands. No live user profile is changed.
`scripts/analyze_run.py SESSION --start SECONDS --end SECONDS` summarizes an
explicit timing window, including percentile statistics and overlap caveats.
The runner freezes the binary, rewritten modules and generated profile tables.
`--host-from` selects a prior binary snapshot for a recorded regression run.
For package validation, `DM_AOC_PROFILE=/existing/absolute/disposable-directory`
selects separate writable state; paths inside the signed app are rejected.

Settings (F1 or Command-comma) offer **Original 30**, **30** and **60 fps**
targets. Original 30 remains the default. The other targets support
**Dynamic step**, which scales the task clock from elapsed scheduler time and
measures the scene/battle clock independently. The event clock follows the scene
cadence when rendering falls behind. Gaps above 100 ms are capped during
loading or focus changes. 60 fps is experimental; achieved rendering/display
rates depend on the scene and screen.

**Known 60 fps cutscene issue:** the first mission's Impa tower cutscene can
run its animation and camera too fast while dialogue/audio remains at normal
speed, even with Dynamic step enabled. The
[FPSLocker configuration for this exact 1.3.0 build](https://github.com/masagrator/FPSLocker-Warehouse/blob/v4/SaltySD/plugins/FPSLocker/patches/01002B00111A2000/C3CF52BF2B05D731.yaml)
also identifies cutscenes whose speed is fixed to 30 fps. Our task/scene clock
telemetry does not validate those animation paths, and disabling 120 does not
resolve this issue. Original 30 is the baseline to compare on replay; an
automatic cutscene fallback has not been implemented or validated. Keep 60 fps
experimental until cutscene entry, playback, audio synchronization and return
to gameplay are tested.

**120 fps is disabled.** Gameplay testing in the first mission, including the
Impa tower cutscene, exposed accelerated game time at that target. The engine
also advances gameplay and script timers in integer 60 Hz ticks; the current
patches do not scale those readers below one tick per update. Correct task and
scene clock telemetry alone does not prove correct game speed. Saved 120 fps
settings and legacy environment overrides fall back to 60. Re-enabling 120
requires addressing these timers and testing gameplay and cutscene transitions.

The independent stats pane offers Off, FPS only and Full stats, plus a frame-time
graph. It reports game presents and actual display FPS separately, frame/GPU
milliseconds, p50/p95/p99 over the last 240 frames, resident shader libraries,
shader inventory, render/compute pipelines, synchronous load/build costs,
imported prewarm objects and load/render-pipeline errors. Prewarm imports are
not a count of shaders actually drawn. Headless runs have no display FPS.
Shortcut hints remain independently configurable.

Stats and the frame graph default to **Off**. Stats-pane timing/display sample
collection is disabled while stats are off. Key 5 or Settings enables stats;
key 7 enables the graph. Explicit choices are saved in `controller-layout.txt`. Environment
overrides `DM_AOC_STATS=0|1|2` (Off/FPS/Full) and `DM_AOC_FRAME_GRAPH=0|1` take
precedence over saved preferences. Disabling/re-enabling collection starts a
fresh sample window. Existing explicit saved preferences are retained.

The frame timer measures time between game presents, not simulation correctness;
actual display FPS comes from drawable presentation callbacks. Shader counters
describe host library/pipeline activity that the game cannot expose through the
Metal HUD. They are not a count of shaders used by a frame. Detailed task/scene
clock logs and periodic renderer summaries require `DM_BOOTPROF=1`; per-frame
call dumps require `DM_NVN_FRAMES=N` or `DM_NVN_TSV=PATH`, and long-frame logging
requires `DM_LONGFRAME=1`. The memory watchdog remains active independently of
diagnostic logging.

For diagnostics, add `--fps 60` to `run_aoc.py`; `--fixed-step`
disables dynamic stepping. `control.txt` also accepts `fps 0|30|60` and
`dynamicfps on|off` for live changes. Direct launches support `DM_FPS_TARGET`
and `DM_DYNAMIC_FPS=0|1`. Selecting a target in settings saves it for later
windowed launches. Rebuild the rewritten modules and host together: the loader
rejects the previous timing ABI.

A sound-heap compatibility patch moves 4 MiB between subheaps without changing their total;
its underlying hardware/work-buffer discrepancy remains unresolved.

## Tests

```sh
# From the repository root (the shared CI suite):
python3 -m unittest discover -s tests -v
# AoC checks, including native ARM64 timing execution on Apple silicon:
make -C games/aoc test PYTHON=/path/to/aoc-python/bin/python
make -C games/aoc test-ui PYTHON=/path/to/aoc-python/bin/python WORK=/path/to/private-ui-tests
make -C games/aoc test-gpu PYTHON=/path/to/aoc-python/bin/python WORK=/path/to/private-probes
```

The tests use synthetic inputs or standalone GPU probes; they do not replace
gameplay testing. This repository contains no game files, keys, saves,
screenshots or translated game shaders/libraries.

The timing tests statically link the rewriter's emitted instructions against
synthetic objects and execute directly on Apple silicon. Only data-page
relocations and exit branches are adapted to the test executable. Native
counter reads and arithmetic are unchanged. They require no CPU emulator,
executable-memory allocation, or game binaries, and skip on other architectures.

## Platform validation

- **macOS / Apple M5, game 1.3.0 (v327680):** signed app built; first-mission
  gameplay played with audio, keyboard and DualSense. Smooth 60 fps gameplay
  was observed, but the Impa tower cutscene has the timing failure described
  above. Later missions, full rendering acceptance and save/reload acceptance
  remain incomplete. The compliance changes are checked with native probes,
  settings tests and a host rebuild; they do not constitute another playthrough.
- **iOS/iPadOS:** no full app build or device playtest. The copied iOS tools lack
  this target's `runtime/ios/Sources` and `runtime/ios/project.yml`; the public
  AoC builder targets macOS. Shared-source SDK compile checks are separate from
  a linked, packaged or playable iOS app. The stats/timing C sources and NVN C/
  Objective-C sources compile-check against the iPhoneOS 27 SDK for ARM64 with
  an iOS 26 deployment target; inherited SDK/Objective-C warnings remain. No iOS
  compatibility is claimed.

The shared root test suite last ran 460 tests successfully (one skipped).
AoC tests and Metal probes must be reported separately from those results and
from gameplay observations. Keep the initial PR marked experimental/draft;
Original 30 is the default, 60 is opt-in, and 120 is disabled.

## Layout

| Path | Purpose |
|---|---|
| `runtime/host` | Native loader, service layer and macOS input/audio front end |
| `runtime/gfx` | NVN/Metal implementation, shader tools and GPU regressions |
| `runtime/tools` | Executable inspection, rewriting and related build tools |
| `runtime/games/01002B00111A2000` | Version-gated profile and game-specific patches |
| `scripts`, `app`, `assets`, `tests` | Build pipeline, launcher, input pins and tests |
| `third_party` | Pinned upstream copies, patches and provenance |
| `prebuilt/macos-arm64` | Verified shader translator payload |

The runtime is based on pinned nxnative. See [COPIES.md](third_party/COPIES.md)
for source provenance and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for
license notices.
