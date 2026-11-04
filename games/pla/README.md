# Pokémon Legends: Arceus (community port, early)

An experiment: run Pokémon Legends: Arceus natively the way `games/totk` runs Tears of the Kingdom. It started from the
Bayonetta 3 port's code (rewriter, loader, Horizon host, NVN on Metal) and keeps its own copies of all of it in this
folder (`runtime/`, `third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`, `scripts/`), so changes here never affect
another game and nothing here reads another game's folder; where each copy came from is recorded in
[third_party/COPIES.md](third_party/COPIES.md). What was found, done and broken on the way: [CHANGELOG.md](CHANGELOG.md).

You must own the game: use your own card image and the `prod.keys` dumped from your own Switch. Nothing from the game
is in this repository; `PLA_build/` (the work folder) is ignored by git.

## Inputs

One card image (XCI) holding **base and update Program NCAs** — base `01001F5010DFA000` (1.0.0) and update
`01001F5010DFA800` (**1.1.1**, the game's final version). The code always comes from the update; its main build ID
`AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E0` is pinned in `assets/expected-v1.1.1.json` and re-checked on every build.

## Build and play

From `games/pla`, with a Python environment made from `requirements.txt` in this folder:

```sh
python scripts/extract_exefs.py "<card>.xci" --keys prod.keys --title 01001F5010DFA000 --update-title 01001F5010DFA800 --output PLA_build/extracted/v1101/exefs
python scripts/extract_exefs.py "<card>.xci" --keys prod.keys --title 01001F5010DFA000 --update-title 01001F5010DFA800 --output PLA_build/extracted/v1101/exefs-base
python scripts/extract_romfs.py --keys prod.keys --title 01001F5010DFA000 --update-title 01001F5010DFA800 --manifest PLA_build/extracted/v1101/extraction-manifest.json --output PLA_build/extracted/v1101/romfs-base
python scripts/merge_romfs.py "<card>.xci" --keys prod.keys --title 01001F5010DFA000 --update-title 01001F5010DFA800 --base-romfs PLA_build/extracted/v1101/romfs-base/romfs.bin --output PLA_build/extracted/v1101/romfs
make prepare            # tables, RomFS index, fonts config (system fonts: see CHANGELOG, stage 4)
make runtime            # PLA_build/build/pla-native
make boot               # windowed boot; the launcher's three switches are commented in the Makefile
```

(The second `extract_exefs` run without `--update` exists to re-write `extraction-manifest.json` for the base NCA,
which `extract_romfs.py` cross-checks; it also re-verifies the base ExeFS block hashes.)

`make shader-tools` puts the prebuilt shader translator (`totk_xlate`, SHA-256 checked) and SPIRV-Cross where
the runtime shader translation looks for it.

Input is keyboard and macOS gamepads (WASD left stick, arrows right stick, J/K/U/I = A/B/X/Y, Q/E = L/R,
Option/Shift = ZL/ZR, Return = +, Tab/Esc = −, F/T/H/G = d-pad). Saves live in `PLA_build/user-data/save/`.

## Status (4 November 2026)

Tested on one Mac: Apple M5, macOS 27.2, Metal 4. The build is a **community port, early**: it boots, loads a save and
plays, but only part of the game has been walked through.

| Works | Details |
|---|---|
| Boot, title, load a save | the newest save loads from the title; `TOTK_AUTOLOAD=1` (test sessions only) loads it without input |
| Play in the field | movement, camera, battles seen in the first area; steady 30 fps (frame time p50 33.3 ms) |
| Saves | `~/Library/Application Support/MacNativeGames/PLA/profile`, the same layout as Tears of the Kingdom |
| Sound | audio plays through the AudioOut path |
| Picture | correct colours and shadows; hard-edged black patches and the half-lit face that appeared in one launch out of three are fixed (CHANGELOG, "Rendering — texture uploads no longer race the guest's writes") |
| Shader seed | `assets/shader_list.tsv` + `pso_manifest.tsv` / `pso_title.tsv`, built with the same tools as Tears of the Kingdom; the first frame waits 3 ms |
| Packaging | `make dist` builds a self-contained app (game data inside the bundle, ~12 GB, deep-signed) |

| Not done or not verified | Details |
|---|---|
| Native 60 fps | the 60 fps track (`PLA_REALTIME=1`, or key 5 in play) makes the game run at **double speed**: part of the simulation advances once per update instead of by dt. The default is the game's own 30 fps step, which is speed-correct. Frame interpolation is not pursued |
| Later areas, long sessions | only the first area and short sessions have been played; crashes from a RomFS read into an unmapped guest page are fixed (CHANGELOG) but long runs are not re-verified |
| Add-on content, world freeze | not implemented; the launcher/uibox show these greyed out |
| iPhone / iPad | not built |
| Shader warm list | grown from play so far (571 variants, 571 shader libraries, 502 render pipelines); most of the game's shaders are still missing, so new areas may compile (or hiccup) on first sight |

Keys in play: key 1 FPS/HUD, key 2 frame interpolation (diagnostic only), key 3 output scaling, key 4 21:9, key 5 the
experimental 60 fps track.
