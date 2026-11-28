# Hyrule Warriors: Age of Calamity on the Zelda-line runtime (bring-up)

Hyrule Warriors: Age of Calamity **1.4.1** (Koei Tecmo's KTGL on NVN, SDK 9.6.0), with the add-on content you own, running
natively on Apple silicon Macs on the same code as Tears of the Kingdom, Animal Crossing, Super Mario Odyssey, Pokémon
Legends: Arceus, Mario Kart 8 Deluxe and Red Dead Redemption: the game's ARM64 code rewritten ahead of time
(`third_party/d2r`), a Horizon host (`runtime/`, much of it linked from `shared/`), and NVN rebuilt on Metal 4
(`third_party/d2r_gfx`). No nxnative code is used: `games/aoc` (this game on nxnative's runtime) was only read for
findings. Where each part comes from: [third_party/COPIES.md](third_party/COPIES.md). What was found and done, and how
each step was checked: [CHANGELOG.md](CHANGELOG.md).

**No game content is in this repository.** You need your own base game NSP, its 1.4.1 update NSP, your add-on NSPs if
you own them, and your own `prod.keys`. The work folder holds game data: never commit or share it.

## Status (22 November 2026, Mac)

| Works | Evidence (screenshots through `TOTK_INPUT_FILE` `shot`) |
|---|---|
| First-start settings (difficulty, voice / text language), "Begin with these settings?", the autosave notice | headless runs, CHANGELOG |
| The opening movies (VP9, the host's libvpx) with sound; + skips them | CHANGELOG |
| The first battle: Link, the soldiers, the HUD and minimap; 30 fps, GPU ~2 ms a frame | `<work>/evidence/5-first-battle-link-hud-minimap.png` |
| The game's own 30 fps (present interval 2) | logs |

| Not yet | |
|---|---|
| Playing the battle through (attacks, the mission's end), the title screen and menus after the prologue, saves past the first-start settings | next |
| The iPhone / iPad app on a device | the app builds (below); not run on a device |
| 60 fps, MNG Launcher card | later; the macOS `.app` package is now supported |

## Build (bring-up)

```sh
WORK=/absolute/path/outside/the/checkout/aocx
PY=/path/to/a/venv/bin/python      # capstone, cryptography, lz4
"$PY" scripts/build_aocx.py --work "$WORK" --base "<base 1.0.0>.nsp" --update "<1.4.1 update>.nsp" --keys prod.keys \
    --dlc "<folder of your add-on NSPs>"
```

or step by step: `games/pla/scripts/extract_exefs.py --update` (the 1.4.1 ExeFS), `scripts/merge_nsp_romfs.py` (the
merged 11.7 GB RomFS), `scripts/extract_dlc.py` (the add-ons into `extracted/aoc/<index>`), `third_party/d2r/patch_nso.py`
on `main`, `subsdk0` and `sdk` (`DM_RULESET=native DM_PRIVATE_GATES=0`), then `make prepare runtime WORK=$WORK PY=$PY`.

## Run a test session

Headless (no window: the picture goes to a layer in no window, screenshots still work), from the work folder:

```sh
cd "$WORK" && TOTK_HEADLESS=1 TOTK_GPU_KEEPALIVE=0 TOTK_GPU_BUDGET_MB=4500 TOTK_PROFILE_DIR=/tmp/aocx-profile \
  TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 DM_PSO_MANIFEST=off \
  TOTK_INPUT_FILE=/tmp/aocx-input.txt ./build/aocx-native rewritten/local boot
```

`TOTK_PROFILE_DIR` keeps the saves out of the player's data folder. Lines appended to the input file drive the game
(`key 38 200`: J = A, `key 36 200`: Return = +, `shot /path.png`: the next frame, `threads`: every guest thread's wait,
`close`: a headless run waits for the GPU to finish and exits). SIGTERM ends a headless run the same way; never kill a
run with SIGKILL in the middle of GPU work (it left the GPU busy and its memory held after the process was gone).
`TOTK_GPU_HANG_S` (default 10) stops the game when the GPU makes no progress that long and lists what was encoded last.
`DM_PSO_MANIFEST=off` skips the warm-up of every seeded shader at start (82,675 libraries).

## iPhone / iPad

As Red Dead Redemption, Mario Kart 8 Deluxe, Odyssey and Arceus: `ios/tools/build_ipa.py` is this game's profile of the
shared port build (`games/totk/ios/tools/build_port_ipa.py`), `ios/Sources/ios_front.m` its answers to the shared shell
(`games/totk/ios/Sources`, `GAME_AOCX` branches), and Zelda's installer installs it. After the Mac build (step 6 of
`scripts/build_aocx.py` makes the icon and the shader seed):

```sh
python3 games/aocx/ios/tools/build_ipa.py --work "$WORK" --out "<folder>/Age of Calamity.ipa" --image-outside \
  --device-gpu applegpu_g14p --device-gpu applegpu_g15p
python3 games/totk/ios/tools/install_device.py "<folder>/Age of Calamity.ipa" --device <name or UDID> \
  --game-data "$WORK/extracted/local/romfs/romfs.bin"
```

What is this game's there:
- **the add-on content** in GameData (`TOTK_AOC_DIR`), as Mario Kart's Booster Course Pass;
- **no font in the app**: the game draws with its own fonts and asks for no system font;
- **its VP9 movies** decoded by the host (the shared `movie_host.c`, libvpx built for the device);
- **its own 30 fps**: no frame-rate choice on the phone; the picture is Odyssey's (the mode and MetalFX to the screen);
- **the vi and oe calls** it makes are answered by the shared phone front end and its own `applet_host.c`;
- **the shader seed** (`scripts/prepare_aocx_shader_seed.py`, `scripts/ktgl_shaders.py`): KTGL keeps its programs as GLSLC
  outputs in the G2S and _DOK containers of its RDB data files (zlib-chunked IDRK entries) and in main: 40,727 shaders,
  each translated for the default bind state and the 25 vertex layouts the Mac met (82,675 variants), by Odyssey's copy
  of the Zelda line's seed tool; every one compiled for the device (`compile_seed.py`).

## Tests

`python3 -m unittest tests.test_aocx` (from this folder; the rewriter tests need capstone and lz4, the add-on ones
cryptography): KTGL's shader containers on synthetic files and the seed tool's game side, the rewriter's w30 jump-table
index and leading patch area, the graphics fixes (a compute unbind keeps the graphics program, buffer copies own their
textures, the copy image stride, c1 segments beyond 4 KB: `nvn_refl.m` compiled and run), the host answers, the headless
layer, the add-on pins, and that the port is on the Zelda line. The repository's `tests/test_hud_panel_shared.py` lists
this game on the shared panel, `tests/test_active_players.py` its start, and `tests/test_ios_ports_unified.py`
(`AocxPort`, `PORTS`) keeps its iPhone / iPad app on the shared shell, build and front end.


## macOS App

The prepared Apple-silicon Mac build can be packaged as a Finder-launchable
`.app`. This does not rebuild the game.

From the repository root:

```sh
python3 games/aocx/scripts/package_app.py \
  --work "$HOME/AoC-Build" \
  --output "$HOME/Applications/Hyrule Warriors Age of Calamity.app"
```

The app contains the native runtime, shader tools, rewritten game modules,
and RomFS indexes. The 11.7 GB `romfs.bin` stays outside the app at:

```text
~/Library/Application Support/MacNativeGames/AOCX/GameData/romfs.bin
```

If this file is missing, packaging installs it from the work folder. If it
already exists, its size and SHA-256 are checked; a mismatching existing file
is never overwritten.

Writable data is kept separately under:

```text
~/Library/Application Support/MacNativeGames/AOCX/
    profile/          saves and settings
    backups/          save backups (five recent starts)
    shader-cache/     Metal shader cache
    game-slot/
    GameData/         external RomFS image
    last-run.log
    previous-run.log
```

The app uses the Mac's preferred language and the game's native AppKit
frontend. The `.app` package being available does not mean the game is
complete: the current bring-up status above still applies. In particular,
the first battle has not yet been verified through its ending, and later
menus and saves remain unfinished.
