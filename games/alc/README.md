# Astral Chain (community port, early)

An experiment: run Astral Chain natively the way `games/totk` runs Tears of the Kingdom. It started from the
Bayonetta 3 port's code (rewriter, loader, Horizon host, NVN on Metal) and keeps its own copies of all of it in
this folder (`runtime/`, `third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`, `scripts/`), so changes here
never affect another game and nothing here reads another game's folder; where each copy came from is recorded
in [third_party/COPIES.md](third_party/COPIES.md). Where the work stands: it is playable — the
prologue and several chapters run at 60 fps with a 1920x1080 internal resolution (both as checked exefs patches),
with the in-game settings panel, remappable keyboard/mouse/controller input, saves and audio; MetalFX frame
interpolation works but still needs polish (see the CHANGELOG's known issues). Earlier stages: the first frame is
presented (`nvnQueuePresentTexture` at 0.2 s), the title flow runs with input, audio output and saves, and the port is
being driven into gameplay. The full log of what was found, done and broken on the way:
[CHANGELOG.md](CHANGELOG.md).

You must own the game: use your own card image and the `prod.keys` dumped from your own Switch. Nothing from
the game is in this repository; `AC_build/` (the work folder) is ignored by git.

## Inputs

One card image (XCI) holding **base and update Program NCAs** — base `01007300020FA000` (1.0.0) and update
`01007300020FA800` (**1.0.1**, the game's final version). The code always comes from the update; its main
build ID `4B159F0F…` is pinned in `assets/expected-v1.0.1.json` and re-checked on every build.

## Build and play

From `games/alc`, with a Python environment made from `requirements.txt` in this folder:

```sh
python scripts/extract_exefs.py "<card>.xci" --keys prod.keys --update-title 01007300020FA800 --output AC_build/extracted/v101/exefs
python scripts/merge_romfs.py "<card>.xci" --keys prod.keys --update-title 01007300020FA800 --output AC_build/extracted/v101/romfs
make prepare            # tables, RomFS index, fonts config (system fonts: see CHANGELOG, stage 4)
make runtime            # AC_build/build/alc-native
make boot               # windowed boot; the launcher's three switches are commented in the Makefile
```

`make shader-tools` puts the prebuilt shader translator (`totk_xlate`, SHA-256 checked) and SPIRV-Cross where
the runtime shader translation looks for it.

Input is keyboard and macOS gamepads (WASD left stick, arrows right stick, J/K/U/I = A/B/X/Y, Q/E = L/R,
Option/Shift = ZL/ZR, Return = +, Tab/Esc = −, F/T/H/G = d-pad). Saves live in `AC_build/user-data/save/`.
Every diagnostic is off by default and switched on by an environment variable; the Makefile comments name the
three the launcher sets.
