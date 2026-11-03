# Pokémon Violet (work in progress)

Pokémon Violet running natively on Apple silicon Macs, the way the other games in this repository run:
- the game's own ARM64 code is rewritten ahead of time and runs directly on the Mac's CPU,
- Horizon's services are answered by a native host,
- NVN graphics are replayed on Metal 4, with the game's shaders translated to Metal.

Looking for an earlier port? See [docs/install.md](../../docs/install.md).

This folder started as a copy of `games/xc3` (Xenoblade Chronicles 3). It keeps its own copies of everything it builds and runs with. Nothing here reads another game's folder. [third_party/COPIES.md](third_party/COPIES.md) says where each copy came from, and [CHANGELOG.md](CHANGELOG.md) what was found on the way.

You must own the game: use your own NSPs and the `prod.keys` and system firmware dumped from your own Switch. Nothing from the game is in this repository. `PSV_build/` (the work folder) and `mods/` are ignored by git.

## Start here

| Task | Section |
|---|---|
| Check what is missing | `make doctor` |
| Build and run the game | [Build and run](#build-and-run) |
| See what works | [Status](#status-1-november-2026) |
| Run it without pressing through the intro | [Test runs](#test-runs) |
| A part is missing or crashes | [Known gaps](#known-gaps) |

## Status (1 November 2026)

Tested on one Mac: Apple M3 Pro, macOS 27, game version 4.0.0 (base NSP 1.0.0 plus the 4.0.0 update).

| Stage | State |
|---|---|
| Code and data | done: 4.0.0 code from the update NSP, RomFS merged from the base and the update, all checked against `assets/expected-v4.0.0.json` |
| Rewrite | done: `main`, `subsdk0` and `sdk` (SDK 13.3.13) pass `structck.py` and `equiv.py --min-eff 8` with 0 divergent sites |
| Boot | done: SDK startup, every constructor, `nnMain`, Wwise sound, the system fonts |
| New game | done: enrollment form (language, name, look), the Uva Academy introduction, first autosave |
| Graphics | the house, the overworld and wild battles render at 60 fps (the game's own 60 fps mode). The grassland holds 120 fps |
| Save and load | the autosave after enrollment loads from the title |
| Sound | Wwise through AudioOut, checked by level (`PSV_AUDIOOUT_LEVEL=1`) |
| Online | answered as offline: BCAT news, Bluetooth accessories (Poké Ball Plus, GO Plus +) |

## Known gaps

- Rare crash at a scene change (`CentralHeap::Free` of a misaligned pointer from gflib's scene manager, about one run in eight). Not reproduced under A/B.
- A hang in the game's job system, about once per 5 to 15 minutes of walking (31 October). Cause open, see the CHANGELOG.
- Stretches of minutes with every tenth frame at about 29 ms in player sessions. Not reproduced in test runs.
- Only one scissor rectangle per draw is replayed. Conservative rasterisation is drawn normally.
- One packaged texture is read back on the CPU because its storage is not in a memory pool.

## Inputs

- Base game 1.0.0: `01008F6008C5E000` (NSP, v0). Update 4.0.0: `01008F6008C5E800` (NSP, v786432). The scripts tell the version by content, never by the file name.
- Your own `prod.keys` (key generation 0x11 or newer for the update).
- The system shared fonts from your own firmware. The game loads them. Without them it stops with a message.

## Build and run

You need:
- Xcode or its Command Line Tools, with the macOS 26 SDK or later. The game needs macOS 26 (`MACOS_MIN` in the Makefile); it has been tested on macOS 27 only.
- From Homebrew: `python@3.14`, `opus`, `libvpx`, and `llvm@23` (for `make verify`).
- git and internet access once: `make shader-tools` fetches SPIRV-Cross at a fixed commit.

From `games/pokesv`:

1. `make doctor` lists what is missing and the target that makes it. Run it again after each step when in doubt.
2. `make venv` makes `PSV_build/venv` (Python 3.14 with `requirements-dev.txt`). Every target uses it once it exists.
3. `make extract BASE=<base>.nsp UPDATE=<update 4.0.0>.nsp KEYS=prod.keys FIRMWARE=<firmware NCA folder>`
4. `make rewrite verify prepare`. The rewrite takes about 1.5 minutes, the check another 1.5. **`make verify` must end with 0 failures and 0 divergent sites on all three modules.**
5. `make -j runtime shader-tools`, then `make boot`.

`make boot` opens the game window and writes its log to `PSV_build/logs/boot.log`, with a copy kept as `play-<date>.log` (test runs overwrite `boot.log`). Saves go to `PSV_build/user-data`. The first start translates shaders as they are met. Later starts load them from the cache.

Keys as in the other ports: WASD the left stick, arrow keys the camera, J is A, K or Space is B, I is X, U is Y, Return is +. A game controller works too. Key 1 opens the settings panel.

### Settings panel

Key 1 or Esc closes it again. Each pick applies at once and is kept in `settings.txt` in the profile folder (`PSV_build/user-data` for `make boot`).

| Row | Choices |
|---|---|
| Frame lock | 30, 60, 90, 120 fps (60 when nothing is saved). **90 and 120 break the game's effects** (they run slow or stop, and the game can hang). The panel says so. |
| Resolution | 1080p, or 1080p with MetalFX upscaling to the window or full screen. |
| Frame interpolation | on or off. MetalFX makes one frame between every two game frames: 60 + 60 on a 120 Hz screen, with the 60 fps lock. About 3.3 ms more GPU per frame and about 5 ms more delay. `PSV_MFX=interp` or `0` decides first. |
| Overlay | the FPS and shader box |

**A higher frame lock does not speed the game up.** The game counts every frame as 33.3 ms. The runtime makes its clocks follow the measured frame time (`runtime/game_patches.c`), so walking is right at 30, 60, 90 and 120. At 30 and 60 the clocks step whole 60 Hz ticks, as on the Switch 2. Above 60 they cannot (a step of 0 ticks stops the game), so effects run slow. `PSV_FRAME_STEP=game` keeps the game's own timing for comparison: then 60 runs twice as fast. In the 60 fps mode two systems keep 30 fps steps on purpose. The first is the fixed-step clock of the game's scripts: they step every frame, so rooms are smooth but scripted fixed-step motion runs twice as fast. Ride jumps get a 1.95 upward scale against that, as in the community mod, and are still shorter than at 30 fps (`PSV_FIXED_STEP=0` turns the clock off and rooms then jerk, `PSV_JUMP_SCALE=0` the jump scale). The second is the interface animations, authored in 30 fps frames (`PSV_UI_SCALE=0` turns that off, and menus and the Pokédex then run twice as fast). `PSV_FAR_DISTANCE=0` keeps the game's animation distance limit of 50000 instead of the mod's 100000. 60 fps is the tested rate, but the frame lock starts at 30 (the game's own rate) until the player picks 60, because of the scripts' faster fixed step. The game's canvas stays 1920x1080: a native higher resolution needs patches to its renderer, which this port does not have yet. At 120 the grassland holds 120 fps on an M3 Pro (1 November 2026). **On "Choose a look for yourself" the cursor starts between the faces: push the left stick once before A.**

### Mods

1. Unpack a RomFS mod into `~/Library/Application Support/MacNativeGames/PSV/mods/v400`, files under their RomFS paths (`world/...`, `arc/...`).
2. Run `make mods`. It checks every file against this RomFS and writes `manifest.tsv`. `make boot` loads that folder (`PSV_ROMFS_MODS`, `0` = none).

Mods made for Trinity Mod Loader work too: for loose files the game keeps in `arc/data.trpfs`, `make mods` writes an `arc/data.trpfd` that sends the game to the loose files. Run `make mods` again after adding or removing files. Tested with Team Seri's "Improved Dynamic LODs" (4.0.0): more models keep their high detail, for about 0.5 ms more GPU time per frame.

### CPU profile

Run `PSV_build/test-tools/prof.sh <name>`: it boots the test profile and records 20 s of Time Profiler (`xctrace`) once in the world. Then export and sum it:

```
xcrun xctrace export --input PSV_build/logs/<name>.trace --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' > PSV_build/logs/<name>.xml
PSV_build/venv/bin/python PSV_build/test-tools/profsum.py PSV_build/logs/<name>.xml PSV_build/logs/<name>.log 40 [<thread id>]
```

Game code shows as `main+0x...` (the log names each module's base). **The .trace holds the shell's environment: delete it after the export.**

## Test runs

These switches make a run need no hands. They are diagnostics and off by default.

| Switch | Effect |
|---|---|
| `PSV_INPUT_SCRIPT=sec:key:ms,...` | presses Mac key codes at set times |
| `PSV_SWKBD_ANSWER=<text>` | answers the name prompt |
| `PSV_IGNORE_GAMEPAD=1` | attaches no controller, so a connected one cannot steer a scripted run |
| `DM_NVN_SHOT=frames:path` with `DM_NVN_SHOT_FRAME=1` | saves the picture at game frames, also with the window hidden (`PSV_WINDOW_SHOT` counts presents, which stop then) |
| `DM_NVN_TEXDUMP=frame:path` | saves every render target of one frame |
| `PSV_FS_LOG=reads` | logs every RomFS open and read |
| `PSV_AUDIOOUT_LEVEL=1` | logs the peak of what the game plays through AudioOut |
| `PSV_SCALED_COPY=0`, `PSV_SCALED_DEPTH_COPY=0` | turn the scaled texture copies off (A/B) |
| Key 0 while playing | saves the game frame as `logs/glitch-<time>.png` and marks it in the log (`[glitch]`). Started with `MTL_CAPTURE_ENABLED=1`, also a Metal capture of that frame (`glitch-<time>.gputrace`, about 2.7 GB). **Apple's replay service crashes on these captures (macOS 27, 1 November 2026):** `gpudebug` lists passes, draws and resources, but cannot fetch textures |
| `PSV_STALL_DUMP=<s>` | seconds without a new frame (window visible) before every guest thread goes to the log. On by default at 5, `0` turns it off |

Source of truth: `assets/expected-v4.0.0.json` for the inputs, `third_party/COPIES.md` for the copied code, the Makefile for the build. Build steps verified 1 November 2026 (rewrite byte-identical, verify clean). Steps 1 to 3 on a fresh Mac are unproven.
