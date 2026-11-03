# Red Alert 2 and Yuri's Revenge: install

[中文](install.zh-CN.md)

Red Alert 2 (`game.exe`, 1.006) and Yuri's Revenge (`gamemd.exe`, 1.001) from your own copy, statically recompiled from
x86 into native arm64 programs for the Mac, and into one iPhone / iPad app holding both. This folder holds the
adaptation code only: the build scripts, the launcher and game-specific hooks, the recompiler's lists and manifests,
tests, and the recompiler itself. **Nothing from the games is in it**: no game code (original or recompiled), no art,
text, sound, MIX files or keys. You supply your own installation, and everything made from it stays on your Mac.

How it works: [x86recomp](https://github.com/JTM-rootstorm/x86recomp) (MIT) lifts every x86 instruction of the game's
executable to C++ (exact x87 and flags model) that compiles to arm64; its runtime answers the Win32 calls (HLE).
DirectDraw is drawn through SDL_GPU on Metal. x86recomp is in `third_party/x86recomp` (upstream `1c90fe3` with our
changes already in, listed in its `UPSTREAM.md`): MMX, DirectDraw v1/v2 2D blits, DirectSound v1, OLE compound-file
saves written from the [MS-CFB] specification, IMM, code pages, touch controls and more. Wine, Bochs and GPL code are
used as behaviour references only, never copied.

## What you need

- A Mac with Apple silicon; Xcode command line tools, `cmake`, `ninja`, LLVM (`brew install llvm`), SDL3, Python 3
  with Pillow.
- Your own Red Alert 2 and Yuri's Revenge, for example the Steam release (app 2229850), installed into `game/install`
  in this folder (`game/` is git-ignored). With steamcmd, logged in to your own account:
  `steamcmd +@sSteamCmdForcePlatformType windows +force_install_dir "$PWD/game/install" +login <you> +app_update 2229850 validate +quit`

## Build

```sh
cd windows_games/ra2
scripts/build.sh            # or: scripts/build.sh <your install folder>
```

It builds x86recomp's lifter, recompiles your executables into `game/generated/` and compiles `build/main/ra2`, and
`build/main/yr` when `gamemd.exe` is in the install too. Running it again rebuilds only what changed (the first build
takes a while: the recompiled code is several hundred MB of C++; about 7 minutes for both games on an M4).
`BUILD_DIR`, `GENERATED_DIR` and `JOBS` (default 6, for 16 GB of memory) change where and how.

The recompiler (`scripts/lift.sh`) checks your executables' SHA-256 against `config/symbols/sha256.txt` and then uses
the function and jump-table lists in `config/symbols/<module>/` (Ghidra's, for the Steam executables; addresses and
flags only, no names). `config/<module>.manifest` lists, with the reason for each, the game functions the launcher
replaces (logging, the frame limiter, the sidebar layout, the tactical view's update) and the recompiler's fixes; it
holds addresses and flags only.

## Another version

The lists are for Red Alert 2 1.006 and Yuri's Revenge 1.001 of the Steam release; with other executables `build.sh`
stops and says so. Export lists of your own with Ghidra 12 (`brew install ghidra openjdk@21`), one per module (`game`
for GAME.EXE, `yr` for gamemd.exe, `blowfish` for Blowfish.dll), and build with them:

```sh
JAVA_HOME=/opt/homebrew/opt/openjdk@21 MAXMEM=6G /opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless \
  game/ghidra ra2 -import game/install/GAME.EXE -overwrite -scriptPath third_party/x86recomp/tools/ghidra \
  -postScript ExportFunctions.java game/symbols/game/functions.tsv \
  -postScript ExportJumpTables.java game/symbols/game/jumptables.tsv
SYMBOLS_DIR=game/symbols scripts/build.sh
```

The game-specific addresses in `config/*.manifest` and `src/` are those of these versions too, so another version
needs them checked as well.

## Run

`build/main/ra2` or `build/main/yr`. The game directory is `--game <dir>`, else `RA2_GAME_DIR`, else the install it was
built from. Options: `--resolution auto|WxH|ini`, `--fps-cap <n>`, `--sidebar-scale <x>`, `--no-command-bar`; the
environment variables `RA2_*` / `YR_*` (such as `RA2_RESOLUTION`) do the same. Files the game writes back (its INI,
the registry) go to an overlay in `~/Library/Application Support/ra2/install/` (`yr` for Yuri's Revenge).
The game runs in a window you can resize (the picture keeps its aspect) and take full screen with its green button;
`RA2_FULLSCREEN=desktop` starts in full screen. The touch controls are on the iPhone and iPad only;
`RA2_SCREEN_CONTROLS=1` shows them on the Mac too.
`scripts/run_attempt.sh <name> [seconds]` runs one attempt with a time limit and keeps the log in `game/attempts/`.

## iPhone and iPad

```sh
scripts/build.sh                   # on the Mac first: the recompiled code
scripts/ios/build_app.sh           # one app, asking which game to play (builds SDL3, glslang, SPIRV-Cross and a
                                   # decoders-only ffmpeg for iOS the first time)
python3 -I scripts/ios/install_device.py --game game/install   # signs with your own team, installs by cable
```

`--game` puts your own install into the app before it is signed, so it is ready to play once installed (about 1.3 GB,
under a minute by cable). The game files in the app are read-only; saves and settings go to the app's Documents and
stay when you install again. The movie archives go in as compact copies made once on the Mac
(`scripts/ios/compact_movies.py`, needs `brew install ffmpeg`, about 10 minutes the first time): every movie's frames
re-encoded from Bink to VP9 and Opus, about 1.1 GB of game files instead of 1.9 GB; `--full-movies` puts in the
originals. An app installed without `--game` reads the install from its Documents folder instead: copy it there as
`RA2` (Finder › iPhone › Files).
The touch controls (stick, group buttons, force fire, cancel) appear in a match. The
app icon is drawn by `scripts/ios/make_icon.py` (original artwork, no game assets or publisher logos).

## Languages

English, 繁體中文 (Traditional Chinese, the Steam release's official one) and 简体中文 (Simplified Chinese, converted from
it). The Chinese ones come from the Steam language pack (depot 2229855; with steamcmd, logged in to your own account:
`steamcmd +@sSteamCmdForcePlatformType windows +login <you> +download_depot 2229850 2229855 +quit`):

```sh
python3 -I scripts/languages.py game/install <the pack's folder>
```

writes `game/install/lang/zh-Hant` (the official string tables as they are) and `lang/zh-Hans` (converted with OpenCC,
`brew install opencc`, and the game font with the glyphs Big5 lacks drawn from Noto Sans CJK SC / Source Han Sans,
SIL OFL 1.1, downloaded at a pinned commit and checked). The game then follows the
system's language; `--language en|zh-Hant|zh-Hans` (or `RA2_LANGUAGE` / `YR_LANGUAGE`) chooses on the Mac, and on the
iPhone and iPad a button in the first dialog (kept for the next start). The MIX public key is read from your own
`game.exe` (`scripts/mixls.py`).

## Tests

`tests/mmx_test.cpp` checks every MMX instruction against values worked out by hand from the Intel manual:
`clang++ -std=c++20 -I third_party/x86recomp/src tests/mmx_test.cpp -o build/mmx_test && build/mmx_test`. x86recomp's
own tests (`ctest --test-dir build/main`) include ours for compound files, code pages, frame layout, touch gestures and
compact movies; `python3 -I -B -m unittest discover -s tests` checks the compact movie archives.

## Status and notes

Both games reach a skirmish match and play on the Mac and on an iPhone 14; saves and loads work on the Mac. Details,
measurements and open problems: [reference/FEASIBILITY.md](reference/FEASIBILITY.md).

Third-party code: x86recomp (MIT, `third_party/x86recomp/LICENSE`), with our changes; it fetches Berkeley SoftFloat 3
(BSD-3-Clause) and CORE-MATH (MIT) when configured. SDL3 (zlib), ffmpeg (LGPL, decoders only), glslang and SPIRV-Cross
are fetched and built by the iOS scripts. See the repository's `THIRD_PARTY_NOTICES.md`.
