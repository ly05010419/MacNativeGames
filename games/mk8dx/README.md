# Mario Kart 8 Deluxe on the Zelda-line runtime (bring-up)

Mario Kart 8 Deluxe **4.0.1** (with the Booster Course Pass, when you own it) running natively on Apple silicon Macs
on the same code as Tears of the Kingdom, Animal Crossing, Super Mario Odyssey and Pokémon Legends: Arceus: the game's
ARM64 code rewritten ahead of time (`third_party/d2r`), a Horizon host (`runtime/`, much of it linked from `shared/`),
and NVN rebuilt on Metal 4 (`third_party/d2r_gfx`). No nxnative code is used (`games/mk8d` was only read for findings).
Where each part comes from: [third_party/COPIES.md](third_party/COPIES.md). What was found and done, and how each step
was checked: [CHANGELOG.md](CHANGELOG.md).

**No game content is in this repository.** You need your own dump of the game, its 4.0.1 update, your own `prod.keys`
and your own Switch firmware (the system fonts and the Mii parts come from it). The work folder holds game data: never
commit or share it.

## Status (21 November 2026, Mac, Apple M4)

| Works | Evidence |
|---|---|
| Boot to the title screen ("Press L R to start", Ver. 4.0.1 with the Booster Course Pass star), its demo race loading | screenshots through `TOTK_INPUT_FILE` `shot` |
| Select a Mii: the six default Miis **with their heads** (the MiiModel parts from your firmware) | screenshot |
| Main menu, mode menu with the playing H.264 preview, VS Race, character select, cups and courses, "Start Race?" | screenshots |
| A VS race at Mario Kart Stadium: driving, HUD, map; 60 fps (p50 16.67 ms) once the shaders are built, GPU ~4 ms (M4) | screenshots, frame log |
| Sound (audio renderer REV4), saves (device and user saves apart), add-on content | logs |

| Not yet | |
|---|---|
| A race seen through to the results; no stutter on first sight | the first race compiles about 800 shaders and pipelines on the spot (a 29 s frame on an empty cache) on the Mac without a seed; the whole-game seed (`scripts/prepare_mk8dx_shader_seed.py`, `build/shader-seed`) serves the phone and is imported by the Mac host at its next start |
| Grand Prix: A on Grand Prix does nothing (VS Race works) | CHANGELOG, open |
| The app (`.app`), MNG Launcher card | next |
| The iPhone / iPad app (built, see below) run on a device | first install 22 November |
| Mii editor (cancelled), online, local wireless | answered as offline |

## Build (bring-up)

```sh
WORK=/absolute/path/outside/the/checkout/mk8dx
PY=/path/to/a/venv/bin/python      # capstone, cryptography, lz4
"$PY" scripts/build_mk8dx.py --work "$WORK" --extracted <folder with ExeFS/ and RomFS.img of base + 4.0.1> \
    --keys prod.keys --firmware <your firmware NCA folder> [--aoc <the Booster Course Pass's RomFS image or files>]
```

or step by step: `scripts/index_romfs_image.py`, `scripts/extract_mii_model.py`, `third_party/d2r/patch_nso.py` on
`main`, `sdk`, `subsdk0` (`DM_RULESET=native DM_PRIVATE_GATES=0`), then `make prepare runtime WORK=$WORK PY=$PY`.

## Run a test session

From the work folder, as the Zelda launchers do (`games/totk/app/launcher.m`):

```sh
cd "$WORK" && TOTK_PROFILE_DIR=/tmp/mk8dx-profile TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 \
  TOTK_GPU_MIDCLEAR=1 TOTK_INPUT_FILE=/tmp/mk8dx-input.txt ./build/mk8dx-native rewritten/local boot
```

`TOTK_PROFILE_DIR` keeps the saves out of the player's data folder. Lines appended to the input file drive the game
(`down 12` / `up 12`: Q = L, `down 14`: E = R, `key 38 300`: J = A for 300 ms, `key 5 300`: G = d-pad down,
`shot /path.png`: the next frame). The keyboard layout is Odyssey's (J/K/I/U = A/B/X/Y, Q/E = L/R, Option/Shift = ZL/ZR,
WASD left stick, arrows right stick, F/T/H/G d-pad, Return = +). `TOTK_MOVIE_LOG=1` logs the menu previews' decoder
queues, `TOTK_INPUT_LOG=1` / `TOTK_INPUT_TRACE=1` the controller reads.

## iPhone / iPad

On the Zelda line's shared app and tools, as Super Mario Odyssey and Pokémon Legends: Arceus: `ios/tools/build_ipa.py`
is this game's profile for `games/totk/ios/tools/build_port_ipa.py` (the shell `games/totk/ios/Sources` with its
`GAME_MK8DX` branches), `ios/Sources/ios_front.m` its answers to the shell, `runtime/frontend_ios.m` the shared front end.
The phone carries the rewritten modules as the Mac leaves them (`export-ios`: no game patch, so no word of the text is
changed), the player's firmware fonts and Mii parts, the Booster Course Pass's files, and no 12 GB image (it is copied
once into Documents/GameData). It cannot translate shaders, so first make the whole-game seed:

```sh
"$PY" scripts/prepare_mk8dx_shader_seed.py --work "$WORK"        # build/shader-seed (Odyssey's seed tool on this game)
python3 ios/tools/build_ipa.py --work "$WORK" --out "<folder>/Mario Kart 8 Deluxe.ipa" --image-outside \
    --device-gpu applegpu_g14p --device-gpu applegpu_g15p          # iPhone 14 (A15), iPad A16
python3 ../totk/ios/tools/install_device.py "<folder>/Mario Kart 8 Deluxe.ipa" --device <name or id> \
    --game-data "$WORK/extracted/local/romfs/romfs.bin"
```

The build merges into the seed what the Mac translated while playing (`build/shader-cache`), compiles every shader for
the device (`compile_seed.py`) and the pipelines the Mac collected for each GPU named (`device_pipelines.py`); the seed
goes beside the IPA (`<name>-shader-seed`) and the installer pushes it once per content.

## Tests

`python3 -m unittest tests.test_mk8dx` (from this folder): the Mii database answers and the add-on content cache size
(`tests/mii_probe.c`), the RomFS image index, the rewriter rows, and that the port is on the Zelda line (no nxnative,
the shared links). The repository's `tests/test_hud_panel_shared.py` lists this game on the shared panel, and
`tests/test_ios_ports_unified.py` keeps its iPhone / iPad app on the shared build (`Mk8dxPort`).
