# Mario Kart 8 Deluxe — native macOS target

[Validation](docs/validation.md) · [Changelog](CHANGELOG.md) · [Source copies](third_party/COPIES.md) · [Licenses](THIRD_PARTY_NOTICES.md)

This directory builds **Mario Kart 8 Deluxe 4.0.1** (title ID `0100152000022000`, update `v1507328`, SDK 4.4.0) into a native Apple silicon Mac app from your own dump. It is a community port, **early**: what has been played is in [Status](#status) and the [validation record](docs/validation.md).

The game's ARM64 code runs natively after an ahead-of-time rewrite; a host answers the Horizon services and maps NVN to Metal. There is no CPU interpreter, JIT or dynamic translation. The runtime, graphics layer, rewriter, shader tools, Mac app packer and game profile are **[nxnative](https://github.com/seedds/nxnative)'s** (seedds, MIT), copied at commit `062cd06` with the changes listed in [COPIES.md](third_party/COPIES.md). nxnative brought Mario Kart 8 Deluxe up: the Mii database, the MiiModel stand-in, SDK 4's audio renderer, the add-on content cache and the two saves are its work, recorded in [its game notes](runtime/games/0100152000022000/README.md). The build wrapper, input pins and tests follow [`games/jamboree`](../jamboree/README.md) (Microbai, PR #684), the first nxnative-based target here, whose host already carried the Mario Kart code.

## Status

Checked on an M1 Max MacBook Pro, macOS 27.0.1, with scripted runs of the built app ([details](docs/validation.md)):

- **Works:** boot to the title with its music, "Select a Mii" and the six default Miis, the menus with their movie previews, a full 50cc Grand Prix race at Mario Kart Stadium to the results, saves written and read again. 60 fps in the race (18,873 of 18,900 frames on screen), about 2.7 GB of memory. With the Booster Course Pass in the input folder, its 12 cups are on the cup screen's next page (without it, choosing one only opens the game's purchase notice), and a scripted 50cc Grand Prix of the Golden Dash Cup ran Paris Promenade, Toad Circuit and Choco Mountain to their results and into Coconut Mall. The scripted race in full screen drops no more frames than in a window (58 and 68 of 18,900). Minus on the character screen (the Switch's controller applet) returns to the game at once; before the [fix of 2026-10-29](docs/validation.md#2026-10-29-the-controller-applet-froze-the-character-screen) it froze the game.
- **Not available:** Mii heads draw nothing (no MiiModel firmware data; bodies and colours show). Online Play shows "Connecting…" and returns to the menu, Wireless Play finds nothing: both are cut on purpose.
- **By hand:** a DualSense became player 1 with its gyro in every session played by hand. On 2026-10-28 a race was played in an unmodified upstream build of the same inputs, and this target's app was only started by hand ([played by hand](docs/validation.md#played-by-hand)). On 2026-10-29 the player played **a whole Booster Course Pass Grand Prix in this target's app, in full screen**, with no problem seen: 61,184 of 61,200 frames on screen in 17 minutes, no audio underrun ([by hand, a Booster Grand Prix](docs/validation.md#by-hand-a-booster-grand-prix-in-full-screen)).
- **Not yet checked:** the other cups by hand, battle mode, multiplayer on one screen, sessions much longer than 17 minutes. The scripted runs press buttons through `DM_KEYS`.

## Build

Use an Apple silicon Mac with macOS 26 or later and Xcode 27.1 with the Metal toolchain (`xcodebuild -downloadComponent MetalToolchain`). The target is Mac only.

Build prerequisites:

- Python 3.12 or later with `lz4`, `capstone` and `fonttools` (and `backports.zstd` before Python 3.14), for example in a virtual environment outside the checkout.
- .NET SDK **10**, `make`, `clang` and `zstd` (Homebrew) on `PATH`. glslang and xcodegen are not needed.
- A **local LibHac 0.19.0 NuGet package**, passed with `--libhac-package`. SHA-256 `1431267861db5ff68216315b164f10c0edab4df162cedf5429bf8a143b76de19` (nuget.org's `libhac.0.19.0.nupkg`). It is a build prerequisite like .NET and the Python packages, not redistributed here. The build never downloads it: `nx_extract` restores from a NuGet configuration that clears every other feed and fallback folder.
- The shader translator is carried as **binaries only** under `prebuilt/`, with SHA-256 checksums and upstream provenance; SPIRV-Cross is built at packaging from its pinned release (fetched from GitHub).
- Your own **base game NSP** and **4.0.1 update NSP** (`v1507328`), and your own `prod.keys`. Optionally, your own purchased **Booster Course Pass NSP** (`0100152000023001`, `v65536`). They are private build inputs; this repository supplies none of them.

Put the NSPs in one folder; other files there are ignored. The base game and the update are told apart by their tickets (else the `[title ID]` in the file name), and the update must be 4.0.1. The Booster Course Pass is taken only by its own ticket, never by a file name. It holds the licence data only (`contents.dat` and `version.txt`): its 48 courses are already in the 4.0.1 update, and the game offers them when the add-on content is there. After extraction, every file of the 4.0.1 ExeFS is checked against its size, SHA-256 and build ID, and the Booster Course Pass's RomFS image against its size and SHA-256, all in [expected-4.0.1.json](assets/expected-4.0.1.json); a filename alone cannot pass. Other updates, other add-on content and XCI or NSZ inputs are outside this target's scope.

From the repository root:

```sh
python3 games/mk8d/scripts/build_mk8d.py \
  /path/to/your/MarioKart8-inputs \
  --keys /path/to/your/prod.keys \
  --libhac-package /path/to/local/libhac.0.19.0.nupkg \
  --work-dir /path/to/private/MK8D-work \
  --output '/path/to/private/Mario Kart 8 Deluxe.app' \
  --language en-US
```

or `make -C games/mk8d build INPUTS=… KEYS=… LIBHAC=… WORK=… OUTPUT=… LANGUAGE=en-US`. The languages are the game's own (`en-US`, `en-GB`, `ja`, `fr`, `de`, `es-419`, `es`, `it`, `nl`, `fr-CA`, `pt`, `ru`, `ko`, `zh-Hant`, `zh-Hans`).

The work folder and the app must be outside the Git checkout. Give a new output path for each build; an existing app is never replaced. Allow about **40 GB** free: the extracted game is 11.3 GB and the app holds a clone or copy of it. The first build compiles 29,651 Mac shaders, which takes most of an hour on an M1 Max; a repeat build reuses every stage whose inputs did not change.

The wrapper stages the runtime, scripts and tools in `<work>/source` and never reads another game's directory. The original NSPs are linked into `<work>/input` (a link whose original has since moved is replaced); extracted files, the Booster Course Pass image (`aoc/1.img`), rewritten modules, logs, shader tables and libraries stay in `<work>/input/0100152000022000_build`, and .NET and temporary files in `<work>/cache` and `<work>/tmp`. The shader tables are made there from your files on every build (they name RomFS paths, so none is committed). Do not edit generated binaries; change the declared rules and rebuild from the original inputs. The built app needs neither Python nor .NET. It is ad-hoc signed and verified, not notarized.

## Launch, controls and saves

To keep the game with your other apps, move the finished app into `/Applications` (in Finder, or `mv`); on the same disk the move is instant and takes no extra space, and the saves stay where they are. Build to a private folder and move the app afterwards rather than giving an `--output` in `/Applications`: the packer keeps its reuse key (`<app>.key`) and, while it builds, a `.partial` app next to the output.

Open the app. A settings window comes first (docked or handheld, upscaling, language, window or full screen, rumble, and a few performance switches); **Play** starts the game. It has no resolution or frame-rate rows: those need rewriter patches this game does not have, and it already runs at 60 fps at its own 1080p docked.

| Keyboard | Game input | In the race |
|---|---|---|
| WASD | Left stick | steer |
| Arrow keys | D-pad | |
| J / K / U / I | A / B / X / Y | accelerate / brake / – / – |
| Q / E / Z / C | L / R / ZL / ZR | item / hop and drift / item / hop and drift |
| Return / Tab | Plus / Minus | pause |
| F / G | Left / right stick click | |

A game controller works through Apple's controller support. Online play and local wireless are cut by the host (the network is always unavailable and local wireless never starts), so the game takes its offline paths.

Private state:

```text
~/Library/Application Support/nxnative/0100152000022000/   save/ (progress), save_device/ (the console's save), backups/, temp/, env.txt, the pipeline list
~/Library/Logs/nxnative/0100152000022000.log              the last run's log (.prev.log the one before)
```

The folder name is nxnative's launcher's; another nxnative-built Mario Kart 8 Deluxe app uses the same saves. Rebuilding the app keeps them.

The game writes into a working copy (`save.work`), which the host swaps in whole when the game commits, so a crash or a forced quit leaves the last committed save. Before each start, the app also keeps the saves as they were in `backups/<date-time>/` (`save/` and `save_device/`), the five newest; the copies are APFS clones and take no space until the game rewrites the saves. The game saves within seconds of every boot (a start count and a time in `userdata.dat`), so each start that reaches the game adds a backup; a start that stops before the game's first save, such as a build that fails at boot, adds none and pushes no older one out. To go back to one, quit the game and copy that backup's `save` and `save_device` folders over the two beside `backups/`.

## Tests

```sh
make -C games/mk8d test
make -C games/mk8d test-gpu WORK=/path/to/private/MK8D-gpu-tests
```

The first runs synthetic build tests (tool checksums, input selection, the title, version and hash gates, offline LibHac) with no game files. The second stages the runtime and runs the copied NVN-on-Metal regressions this game needed (`ALL=1` for the whole suite) on the Mac's GPU, with no game data. Neither replaces playing.

## Layout

| Path | Purpose |
|---|---|
| `runtime/host` | Loader, Horizon services and the Cocoa / GameController front end |
| `runtime/gfx` | NVN on Metal, the shader tools and their GPU regressions |
| `runtime/tools` | NSO reader, ahead-of-time rewriter, `nx_extract` (LibHac), font builder |
| `runtime/mac`, `runtime/ios/Sources` | Mac app packer, launcher and settings window (with the two launcher sources it links) |
| `runtime/games/0100152000022000` | The game profile (4.0.1), its prewarm list and nxnative's game notes |
| `runtime/third_party` | libvpx (VP9), stb_vorbis, Vulkan headers, MacNativeGames license |
| `prebuilt/macos-arm64` | Locked shader translator binaries with checksums, SPIRV-Cross's licenses; no game shaders |
| `third_party/nxnative` | Pinned upstream build orchestrator with this target's gates, license, import manifest and local patch |
| `scripts`, `assets`, `tests` | Build wrapper, GPU test runner, input pins, synthetic tests |
