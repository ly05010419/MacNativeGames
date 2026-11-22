# Red Dead Redemption on the Zelda-line runtime (bring-up)

Red Dead Redemption **1.0.4** (Rockstar's RAGE in Double Eleven's Switch port, SDK 15.3.2, NVN) running natively on
Apple silicon Macs on the same code as Tears of the Kingdom, Animal Crossing, Super Mario Odyssey, Pokémon Legends: Arceus
and Mario Kart 8 Deluxe: the game's ARM64 code rewritten ahead of time (`third_party/d2r`), a Horizon host (`runtime/`,
much of it linked from `shared/`), and NVN rebuilt on Metal 4 (`third_party/d2r_gfx`). No nxnative code is used
(nxnative's notes on this game were only read for findings). Where each part comes from:
[third_party/COPIES.md](third_party/COPIES.md). What was found and done, and how each step was checked:
[CHANGELOG.md](CHANGELOG.md).

**No game content is in this repository.** You need your own card image of the game with its 1.0.4 update (base and
update in one file, "1G+1U") and your own `prod.keys`. The work folder holds game data: never commit or share it.

## Status (22 November 2026, Mac, Apple M4)

| Works | Evidence (`<work>/evidence`) |
|---|---|
| Language selection, the three documents (license agreement, Terms of Service, Privacy Policy) accepted, subtitles prompt | `0-language-and-three-documents.png` |
| Health warning, the Rockstar Games logo movie (Bink, its own audio renderer), Double Eleven's logo, the title ("Press +") | `1-rockstar-logo-movie.png`, `2-title-press-plus.png` |
| Difficulty menu, the "no save" alert (a new save is created), the autosave warning | `3-difficulty-menu.png`, `4-no-save-alert.png`, `5-autosave-warning.png` |
| The opening: Blackwater, John Marston and the agents, the train to Armadillo with the credits | `6-…`, `7-…`, `8-opening-train-credits.png` |
| Control of John Marston at Armadillo Station ("Go to the saloon"), HUD and minimap, walking and turning the camera | `9-control-armadillo-station.png`, `11-walk-to-the-station.png`, `12-camera-turned.png` |
| Saves (`PREBOOT.SAV` keeps the agreements, `RDR2SAVE0.SAV`, `RDR2OPTIONS.SAV`), sound | logs |
| The game's 30 fps: frame time p50 33.3 ms, p95 33.5 ms; GPU p50 5.7–6.3 ms | `*-run.log` |

| Not yet | |
|---|---|
| Glass (windows, windscreens) drawn bright green; black blocks on some distant foliage and buildings | open (CHANGELOG) |
| 60 fps (nxnative's finding: present interval 2 is the game's only limit), the app (`.app`), MNG Launcher card | next |
| iPhone / iPad | the app builds (`ios/`, "iPhone / iPad" below); not yet started on a device |
| 1.0.6 | its card image is not on this Mac; 1.0.4 only |

## Build (bring-up)

```sh
WORK=/absolute/path/outside/the/checkout/rdr
PY=/path/to/a/venv/bin/python      # capstone, cryptography, lz4
"$PY" scripts/build_rdr.py --work "$WORK" --game "<your base + 1.0.4 card image>.xci" --keys prod.keys
```

or step by step: `games/pla/scripts/extract_exefs.py --update` (the 1.0.4 ExeFS), `scripts/merge_card_romfs.py` (the
merged 11.2 GB RomFS, the base read from the card), `third_party/d2r/patch_nso.py` on `main` and `sdk`
(`DM_RULESET=native DM_PRIVATE_GATES=0`), then `make prepare runtime WORK=$WORK PY=$PY`.

## Run a test session

From the work folder, as the Zelda launchers do:

```sh
cd "$WORK" && TOTK_PROFILE_DIR=/tmp/rdr-profile TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 \
  TOTK_GPU_MIDCLEAR=1 TOTK_INPUT_FILE=/tmp/rdr-input.txt ./build/rdr-native rewritten/local boot
```

`TOTK_PROFILE_DIR` keeps the saves out of the player's data folder. Lines appended to the input file drive the game
(`key 38 200`: J = A, `key 36 200`: Return = +, `down 125` / `up 125`: arrow down = right stick down, `down 13` /
`up 13`: W = left stick up, `shot /path.png`: the next frame, `close`). A fresh profile's route: A on the language
(about 20 s in); on each document hold arrow down until its end, then A (the A is refused before the end); A on the
subtitles prompt; the title about 100 s in. Once `PREBOOT.SAV` holds the agreements, the title comes in about 45 s;
then + (Return), A on Normal, A on "Create a new save game", A on the autosave warning; a saved game continues at
Armadillo Station.

## iPhone / iPad

As Mario Kart 8 Deluxe, Odyssey and Arceus: `ios/tools/build_ipa.py` is this game's profile of the shared port build
(`games/totk/ios/tools/build_port_ipa.py`), `ios/Sources/ios_front.m` its answers to the shared shell
(`games/totk/ios/Sources`, `GAME_RDR` branches), and Zelda's installer installs it. After the Mac build (step 5 of
`scripts/build_rdr.py` makes the icon and the shader seed):

```sh
python3 games/rdr/ios/tools/build_ipa.py --work "$WORK" --out "<folder>/Red Dead Redemption.ipa" --image-outside \
  --device-gpu applegpu_g14p --device-gpu applegpu_g15p
python3 games/totk/ios/tools/install_device.py "<folder>/Red Dead Redemption.ipa" --device <name or UDID> \
  --game-data "$WORK/extracted/local/romfs/romfs.bin"
```

What is this game's there:
- **main's 1.2 GiB .bss and 16 MiB leading patch area**: the shared `gen_embed.py` embeds the file's three parts (the
  patch area, the exported text, the data up to the end of the file: 45 MB of code, 24 MB of data); the loader's
  iPhone path (`map_module` under `TARGET_OS_IPHONE`) reserves patch area, image and key page in one zero-filled range
  and maps the parts back, so .bss is not in the app (an 81 MB IPA). No extended virtual addressing: the reservation
  is 1.3 GB, and the guest regions after it shrink their alias part to what is left (`services.c` `memory_init`).
- **the vi calls** this game makes (the sized `CreateLayer`, `SetLayerCrop`, `DestroyLayer`, `CloseDisplay`,
  `vi::Finalize`) are answered by the shared phone front end (`shared/runtime/frontend_ios.m`), as by `runtime/frontend.m`.
- **no font, no movie library in the app**: the game draws with its own fonts and never asks for a system font (on the
  Mac neither); main decodes its Bink movies (only libopus is built for the device).
- **its own 30 fps** (present interval 2): no frame-rate choice on the phone; the picture is Odyssey's (the mode and
  MetalFX to the screen). Asynchronous preemption on, as on its Mac (`shared/runtime/threads.c`).
- **the shader seed** (`scripts/prepare_rdr_shader_seed.py`, `scripts/rpf6.py`): RAGE keeps its NVN programs in its
  own records in the 128 effects of `shaders.rpf` (an RPF6 archive of Zstandard frames; the encrypted entry table is not
  needed) and in main: 3,581 shaders (3,413 + 168), each translated for the default bind state and for the 12 vertex
  layouts the Mac met (6,754 variants, NVN's 0..1 depth), by Odyssey's copy of the Zelda line's seed tool. The 672
  variants the Mac translated to Armadillo: 670 are in the seed byte for byte, 2 (464-byte shaders the game builds at
  run time) come from the Mac's cache (`build_port_ipa.py` merges it). Every shader is compiled for the device
  (`compile_seed.py`: 7,426 libraries) and the 452 pipelines the Mac built for each GPU (`device_pipelines.py`).

## Tests

`python3 -m unittest tests.test_rdr` (from this folder; the rewriter tests need capstone and lz4, the seed reader's
zstandard): the shader seed's RPF6 and program reader and the seed tool's game side on synthetic archives, the leading patch
area on synthetic modules (`tests/synthetic_nso.py`), the card RomFS merge's base reader and FS header check, the
host answers this game needs and the loader's symbol order, and that the port is on the Zelda line (no nxnative, the
shared links). The repository's `tests/test_hud_panel_shared.py` lists this game on the shared panel,
`tests/test_active_players.py` its start, and `tests/test_ios_ports_unified.py` (`RdrPort`, `PORTS`) keeps its iPhone /
iPad app on the shared shell, build and front end (the leading patch area through the shared `gen_embed.py` included).
