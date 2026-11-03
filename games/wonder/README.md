# Super Mario Bros. Wonder (early port)

Super Mario Bros. Wonder 1.0.0 (`010015100B514000`), or 1.2.1 with its update, running natively on Apple silicon Macs, built from **your own copy**
of the game. It started from the Tears of the Kingdom host (rewriter, loader, Horizon services, NVN on Metal) and keeps
its own copies of all of it in this folder (`runtime/`, `third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`,
`scripts/`), so changes here never affect another game and nothing here reads `games/totk`. One game-code patch: the
real-time frame step, so the game keeps its speed at any frame rate (`TOTK_REALTIME=0`: as shipped, held at 60 fps).

**No game content is in this repository.** You need a legal dump of your own game and keys from your own Switch.

## Build

Needs a Mac with Apple silicon and macOS 26 or later, the Xcode command line tools, Python 3.14, and from Homebrew
`opus libvpx`.

```sh
cd games/wonder
python3.14 scripts/build_wonder.py --game "<your 1.0.0 game>.nsp" --keys "<your prod.keys>" --firmware "<unpacked firmware NCA folder>" \
    [--update "<your 1.2.1 update>.nsp"]
```

- The game file is the 1.0.0 base game, NSP or XCI. An NSP carries no system fonts: pass `--firmware`, or
  `--shared-fonts <folder>` with a font folder an earlier build verified (`extracted/firmware/shared-fonts` in any of
  this repository's work folders). Every step is checked against `assets/expected-v1.0.0.json` (hashes only).
- `--update` takes the 1.2.1 update (`010015100B514800`, v327680): the app then runs 1.2.1, its code from the update and
  its RomFS the base game's merged with the update's (`scripts/merge_romfs.py`, all six IVFC levels verified), checked
  against `assets/expected-v1.2.1.json`. The first start of 1.2.1 copies your saves to `profile-before-1.2.1` in the data
  folder, as 1.2.1 may upgrade them.
- A fresh build takes a few minutes (3.7 GB of game data); a second build reuses every verified step.
- The work folder is `Wonder_build` next to the game file (`--work` to choose another). It holds game data: never share
  or commit it. The app `Super Mario Bros. Wonder.app` goes next to the game file (`--out` to choose).
- Saves, the shader cache and the log are in `~/Library/Application Support/MacNativeGames/Wonder` and
  `~/Library/Logs/MacNativeGames/wonder.log`; a build never touches them.
- Mac only: no iPhone / iPad build.

## What is here

| File | What it is |
| --- | --- |
| `assets/expected-v1.0.0.json`, `expected-v1.2.1.json` | the hashes every build step is checked against (ExeFS, the two rewritten modules, the RomFS) |
| `scripts/build_wonder.py` | the one-command build; it copies this folder into the work folder and runs the steps there |
| `Makefile` | the game binary (`make binary`, `build/wonder-native`; `VERSION=wonder121`: `build/wonder-native-wonder121`, with `WONDER_V121`), built from this folder alone, and the game data folder it reads at start (`make game-data`, `build/game-data/<version>`: module tables, RomFS and font hashes; `make runtime` makes both) |
| `runtime/game_patches_wonder.c` | the game's patch file, in place of TOTK's: the real-time frame step (its sites per version, found in the game's own code); exclusive loads take acquire (see the file) |
| `runtime/offline_host.c` | the services an offline console answers for: network (nifm, sockets, ssl, nsd), friends, local wireless, play reports, the console identity |

Forked from `games/totk` (commit `33e8fa8`, 2026-10-25; brought up to date with it at `1dcf40d`, 2026-10-28: the settings panel, frame lock,
mouse, controller and graphics fixes, and the game data folder) with the game's changes: its block in `runtime/game_version.h`
(`GAME_WONDER`), user selection and the profile image (`account.c`), the FS context and the game's own save-data calls
(`fs_host.c`), a 60 Hz VI vsync on any display (`frontend.m`), the Nintendo Extension font handed over with its BFTTF
header (`shared_font.c`), and bindless texture handles read from uniform blocks (`third_party/d2r_gfx`). The scripts
take the title ID (`extract_exefs.py --title-id`) and the modules a title ships (`prepare_modules.py`, `prepare_runtime.py`).
1.2.1 (SDK 16.2.12) opens the user the system opened at launch (`nn::account::PopOpenUsers`, as Breath of the Wild 1.9.0),
answered in `account.c`; `scripts/merge_romfs.py` is games/ssbu's copy.

## Status

Tested on a Mac with a 120 Hz display (October 2026): 1.0.0 title screen, user selection, a new game and the first level
with the HUD, with sound; 1.2.1 into a level at 120 fps with the real-time frame step (game time = real time). Not yet:
a startup shader set (each shader is translated the first time it is drawn, then cached), iPhone / iPad.
