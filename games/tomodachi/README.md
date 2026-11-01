# Tomodachi Life: Living the Dream — experimental native Mac port

**Status: draft / work in progress. Broader gameplay and visual validation remain.**
The game starts, native menus and text entry work, and an existing island has
been exercised on an Apple silicon Mac. This is not a claim of complete picture
accuracy, full gameplay coverage, or proven save compatibility. Keep a backup of
your own saves. No iPhone/iPad build is provided or validated.

This port runs the player's ARM64 game code directly on Apple silicon. It uses
ahead-of-time NSO rewriting, a native service host, and NVN on Metal; no CPU
interpreter, JIT, or emulator backend is used by the port.

## Supported input and requirements

- Game: **Tomodachi Life: Living the Dream 1.0.0**, title ID `010051F0207B2000`.
  Only the identities in `assets/expected-v1.0.0.json` are accepted.
- Your own original game dump (XCI/NSP), your own `prod.keys`, and your own
  firmware/shared fonts. No game files, keys, fonts, shaders, saves, or app icons
  are included in this folder. The icon is extracted locally when packaging.
- Apple silicon Mac, Xcode command-line tools, **Python 3.14**, and Homebrew
  `opus` and `libvpx`. Metal 4 requires macOS 26+ and an SDK providing Metal 4.
- Metal 3 remains an explicit compatibility backend. Both backends build,
  package, and pass the loader probe on the development Mac; macOS 15
  playability has not been verified on an actual macOS 15 system.
- An externally built SPIRV-Cross executable. Use the pinned upstream revision
  `aa217ae` from <https://github.com/KhronosGroup/SPIRV-Cross> (Apache-2.0).
  Build it in a directory outside this source checkout.

Every required runtime, graphics, rewriting, and shader-service source is local
to this folder. See [copy provenance](third_party/COPIES.md) and
[dependency licenses](THIRD_PARTY_NOTICES.md). Builds do not import another
game's runtime or scripts. The prebuilt shader translator is a necessary native
build tool, not a game binary; its SHA-256 is checked before use.

## Prepare and build

Run from the repository root. All paths below are examples; put game data,
intermediate files, and the resulting app **outside the checkout**.

```sh
python3.14 -m venv /path/to/Tomodachi_build/venv
/path/to/Tomodachi_build/venv/bin/python -m pip install -r games/tomodachi/requirements.txt
/path/to/Tomodachi_build/venv/bin/python games/tomodachi/prepare.py \
  /path/to/your-game.xci --keys /path/to/prod.keys \
  --work /path/to/Tomodachi_build/prepared --romfs
```

Extract the firmware's shared fonts into an external directory, or provide the
six verified fonts and manifest from your own existing extraction. No firmware
is obtained by these scripts.

```sh
/path/to/Tomodachi_build/venv/bin/python games/tomodachi/tools/extract_shared_fonts.py \
  --firmware /path/to/your-firmware --keys /path/to/prod.keys \
  --output /path/to/your-shared-fonts
```

```sh
/path/to/Tomodachi_build/venv/bin/python games/tomodachi/build_host.py \
  --prepared /path/to/Tomodachi_build/prepared \
  --work /path/to/Tomodachi_build/host \
  --shared-fonts /path/to/your-shared-fonts \
  --spirv-cross /path/to/spirv-cross --backend metal4

make -C games/tomodachi WORK=/path/to/Tomodachi_build/host BACKEND=metal4
/path/to/Tomodachi_build/venv/bin/python games/tomodachi/package.py \
  --work /path/to/Tomodachi_build/host --backend metal4 \
  --output '/path/to/apps/Tomodachi Life.app' --language zh-Hant
```

Use `--replace` to archive an earlier app only after the new package is complete
and its signature verifies. Keep the external host and prepared game data: the
app records relative paths to them. Moving the app alone can break those paths.
To rebuild the optional Metal 3 backend, use `BACKEND=metal3` and package with
`--backend metal3`. Do not point build output into this or another source
worktree. Run `--help` for each script's full input contract.

## Controls and preferences

Press **1** or **⌘,** for Settings. Display controls window mode, Retina/display
quality, the optional FPS graph, and game language (next launch). Input controls
controller family, face-button layout, and vibration. **Key bindings** reuses
the upstream keyboard/controller drawings: click a key or physical button to
choose its action. Reset keyboard and controller layouts independently.

Default keyboard controls: J confirms, K/Space goes back, I is X, U is Y,
WASD moves, arrows drive the right stick, Return is Plus, Tab/Escape is Minus.
The mouse remains touch input; its buttons are not reassigned to Zelda actions.
L3+R3 toggles Settings; L/R changes tabs; d-pad/stick navigates; B closes it.
Controller navigation was covered with simulated events; this draft does not
claim a hardware sweep across Xbox, PlayStation, Switch, and reconnect cases.

Preferences live in the native profile's `settings.json`. Existing ten-key and
face-swap preferences migrate into the diagram mappings. Settings captures guest
input while open; it does **not** pause the simulation. Native text entry returns
focus and callbacks to the requesting guest context. `InlineKeyboard::Calc`
returns the SDK keyboard state; the lifecycle test checks initialized, shown,
completed, and finalized states. Submission and cancellation do not emit the
applet-exit callback, which would reset the title's pending name. A live Metal 4 Mii name prompt was verified to
accept typing and update the guest preview without repeatedly reopening. After
correcting the submit callback sequence, the name remained on the in-game name
screen with OK enabled after the native field closed. This checks accepting the
edit, not save persistence across restarts.

The regular app profile is `~/Library/Application Support/MacNativeGames/Tomodachi/profile`.
The port never reads or modifies an emulator's profile or save automatically.
Packaged runs keep diagnostics off by default. Set `TOMODACHI_DEBUG_LOG=1` when
launching the executable to append a native runtime log to that profile.

## Validation and limitations

- Metal 4: compiled and packaged on an Apple silicon Mac running macOS 27.2;
  native setup/menus, controller/settings drawings, text input, and island
  interactions have been exercised. Existing local saves were used only with
  separate native copies during development.
- Fresh self-contained Metal 3 and Metal 4 builds passed the native cross-module loader
  probe: 4,000 calls across four threads. This checks loading/linkage, not gameplay.
- The reproduced flickering highlight patches on Mii faces, hair, and clothing
  were corrected by depth-feedback snapshots. Same-process on/off/on comparisons
  isolated the cause; this does not establish picture accuracy in every scene.
  Full island gameplay, long sessions, and save round-trips require more testing.
  Photo capture has separately hit an unsupported geometry-stage shader.
  Clicking a Mii dream bubble once triggered a null shader-entry abort. The
  bubble disappeared before the instrumented retry, so the cause is unresolved;
  subsequent play without another crash is not a verified fix.
- Some services are partial implementations; some audio effects remain unsupported.
  The runtime expands verified Mii/cloth child heaps and aligns physical NVN pool
  backing for Darwin pages; these guarded compatibility patches need review.
- No performance improvement is claimed from local work counters or FPS alone.
  Rendering correctness takes priority over speed. Keep this contribution a
  draft until the remaining graphics and gameplay validation are complete.

Run the parent repository suite and the title-local tests:

```sh
/path/to/Tomodachi_build/venv/bin/python -B -m unittest discover -s tests
/path/to/Tomodachi_build/venv/bin/python -B games/tomodachi/tests/run_tests.py
# Optional native-window checks on a local Mac desktop:
/path/to/Tomodachi_build/venv/bin/python -B games/tomodachi/tests/run_tests.py --ui
```

The tests use synthetic inputs and temporary directories outside source. They
cover NSO PC-relative relocation, packaging failure preservation and final path
resolution, timezones/DST, gestures, native keyboard callbacks, preference
validation, guest input remapping, and controller settings navigation.
`run.py --mode probe` is a bounded local-data loader diagnostic; `--mode startup`
and `--mode boot` perform additional native checks with isolated profiles. They
need your prepared host and must not run alongside another game.

## Character highlight regression

The lighting shader sampled the same depth image used as its live attachment.
The renderer now lands pending clears and samples an immutable depth copy. Reuse
is limited to an unchanged read-only pass; pass boundaries, depth writes, and
source replacement force a fresh copy. The change defaults on only for this
title. Current snapshot support is single-level, single-sample 2D textures.
Extra copies/pass boundaries have a cost; no performance improvement is claimed.

On the reproduced lineup, snapshots on (8 frames), off (16 frames), then on
again (24 frames) removed, restored, then removed the block-shaped highlights.
Metal 3 and Metal 4 each passed 3,117 synthetic GPU checks with Metal validation.
Disabling snapshots made the regression fail at the live-attachment assertion.
Run this optional check with a built host and your own rewritten modules, with
all games closed:

```sh
MTL_DEBUG_LAYER=1 /path/to/host/build/tomodachi-native-metal4 \
  /path/to/host/rewritten probe-gpu-depth-feedback
# Metal 3: use build/tomodachi-native instead.
# Negative control (expected failure): add TOMODACHI_DEPTH_SNAPSHOT=0.
```

The probe uses synthetic shaders and textures; game shaders and saves are not
part of the test source. This result covers the reproduced highlight defect,
not all Mii rendering or the separate unsupported photo shader.

The current contribution scope, per-platform evidence, and remaining gaps are
recorded in [validation notes](docs/validation.md).
