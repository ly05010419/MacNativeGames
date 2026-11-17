# The Legend of Zelda: Echoes of Wisdom (community port, early)

Echoes of Wisdom 1.1.0 running natively on Apple silicon the way `games/totk` runs Tears of the Kingdom: the game's own
ARM64 code, rewritten ahead of time, a small Horizon host and NVN rebuilt on Metal. It started as a copy of `games/by3`
and keeps its own copies of what it builds and runs with ([third_party/COPIES.md](third_party/COPIES.md)), except the
code every game uses: the FPS box, the settings panel and its pages come from [`shared/`](../../shared/README.md)
through links.

**Playable from the title into the game on the Mac**: the title screen with its movie, the prologue (Link in the dungeon,
HUD, Chinese text), sound and music, keyboard and game controllers, saves. 60 fps as on the Switch. How it was done,
what was found and what is still open: [CHANGELOG.md](CHANGELOG.md).

You must own the game: use your own NSPs and the `prod.keys` dumped from your own Switch. Nothing from the game is in
this repository; the work folder (`EoW_build`) and `mods/` are ignored by git.

## One command, an app, or MNG Launcher

`build.py` runs the steps below in order, reuses what an earlier run in the same work folder made from the same files,
and packages **The Legend of Zelda Echoes of Wisdom.app**:

```sh
python3.14 games/eow/build.py base.xci -k prod.keys --update update-1.1.0.nsp \
  [--firmware <unpacked firmware> | --shared-fonts <fonts folder of an earlier build>] [--work <folder>] [-o <app>]
```

The base may be the card image (XCI) or the NSP; the update needs key generation 17 in `prod.keys` (firmware 20 or
newer). A card image whose system update partition lacks the fonts needs `--firmware` or `--shared-fonts`. The shaders
are compiled with Xcode's Metal compiler (taken from `/Applications/Xcode.app` when `xcode-select` points at the command
line tools). Every step logs to `<work>/logs/`; the app records its work folder. On an M4 MacBook Air a build that
extracted and merged the game data again and reused the rewrite took 5 minutes; the shader seed alone takes about 4.

The app (`app/launcher.c`, `scripts/package_app.py`) carries the runtime, the rewritten modules, the RomFS (an APFS
clone of the work folder's), the fonts, the translator, the shader seed and the compiled libraries, all checked before
packaging. It is signed ad hoc with its bundle ID `org.macnativegames.eow` as the designated requirement. The player's
data is in `~/Library/Application Support/MacNativeGames/EoW` (`profile/` saves, `shader-cache/`, `last-run.log`); the
game's language follows the Mac's first preferred language. MNG Launcher builds it from the dropped files and updates it
without them (`update_runtime.py` calls `build.py`'s `update`); its card has the Controls, Storage and Saves pages.

## Inputs

- Base `01008CF01BAAC000` (v0) and update 1.1.0 `01008CF01BAAC800` (v196608), both NSP.
- The RomFS sections use NCA **compression** (LZ4 ranges in a bucket tree), which the pinned hactool cannot read;
  `scripts/merge_romfs.py` reads it. All code comes from the update.
- The six system fonts from your own Switch firmware in `$WORK/extracted/firmware/shared-fonts` (as the TotK build
  extracts them; the game draws its text with them).

## Build

From `games/eow`, with a Python environment made from `requirements.txt` plus `unicorn` (for `equiv.py`); `PY=` that
python, `WORK=` the work folder (an APFS volume; about 25 GB):

```sh
python scripts/extract_code.py "<update>.nsp" --keys prod.keys --output $WORK/extracted/v110/exefs
python scripts/merge_romfs.py "<base>.nsp" "<update>.nsp" --keys prod.keys --output $WORK/extracted/v110/romfs
for m in sdk subsdk0 subsdk1 main; do
  DM_RULESET=native DM_PRIVATE_GATES=0 $PY third_party/d2r/patch_nso.py $WORK/extracted/v110/exefs/$m $WORK/rewritten/v110 $m
done
make prepare runtime shader-tools PY=$PY WORK=$WORK
make shader-seed PY=$PY WORK=$WORK DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer   # about 1.5 minutes
```

`assets/expected-v1.1.0.json` pins every resulting hash; `make prepare` checks them.

### Shaders

`make shader-seed` prepares every shader ahead of play, from your own game files:

- `scripts/prepare_shader_seed.py` finds the game's 9,858 shaders (the BFSHA archives, the effect files, the UI layout
  archives and the main module; the count is checked) and translates each for the default bind state, plus every bind
  state `assets/shader_list.tsv` lists (states met in play that the default variant does not serve; identifiers only,
  no game data; format in `scripts/shader_list.py`). Two pixel shaders of `postprocess.bfsha` are left out (the translator's output is rejected by
  SPIRV-Cross; never requested in recorded play); `seed-manifest.json` records each with its reason and result.
- It then compiles every MSL of the new seed into Metal libraries (`scripts/build_shader_libs.py`). Seed and libraries
  are built beside the published ones and published as a pair only when both are complete, libraries first, each with
  one exchange of the folder names (`$WORK/build/shader-libs`, then `$WORK/build/shader-seed`); the old pair is kept
  until both exchanges are done, and a failure or Ctrl-C at either puts the old pair back. Any failed translation or
  compile leaves the published pair as it was. A run killed between the two exchanges leaves the new libraries with
  the old seed: the same compiler's new libraries hold every old one, after a compiler change play compiles what they
  lack; `--check` names it and `make shader-seed` repairs it. `build_shader_libs.py --check` says whether the libraries serve the seed; `make boot` runs it and warns.
- The runtime imports the seed into its shader cache at the next start: once, before the intro movie (that start
  reaches the movie in about 12 s on an M5 Pro, later starts in about 1.5 s).
- New bind states met in play (the log's `[shader] translated` lines) join the list with
  `$PY scripts/prepare_shader_seed.py --work $WORK --list-from $WORK/build/shader-cache/rt-totk-native-v4-unverified`.

## Play

From the work folder:

```sh
$PY <repo>/games/eow/scripts/build_shader_libs.py --work .     # shaders met in earlier sessions, compiled ahead (seconds)
TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BUDGET_MB=5000 \
  DM_PRESENT_LINK_FALLBACK=1 ./build/eow-native rewritten/v110 boot 2> logs/play.err
```

- Click the window once: macOS gives controller input only to the active app, and a window started from a terminal is
  not always activated.
- Keyboard and controllers: see [Keys](#keys); key 1 (or L3 + R3 on a controller) opens the [settings panel](#settings-panel).
- Saves: `user-data/` in the work folder. Close the window to quit; the game finishes its save first.
- Stutter: with `make shader-seed` every shader is translated and compiled before play; without it, shaders are
  translated once and cached, and `build_shader_libs.py` compiles them ahead of time into `build/shader-libs`, so only
  shaders never seen before are compiled during play (0.3-0.6 s each, once).

## Settings panel

Key 1, or L3 + R3 pressed together on a controller, opens and closes it; Esc and its ✕ close it too. While it is open the
game gets no keyboard, mouse or controller input. It is the Zelda games' panel (`shared/runtime/settings_panel.h`) with
this game's rows (`runtime/settings_game.h`); every row applies at once and is kept next to `controls.txt` in the data
folder, so it holds for the next launch. A row this game cannot change shows its one value, greyed out.

- **Display**: the overlay (all lines; FPS full information, which adds every long frame ("hitch") with what it spent
  its time on, the shader libraries and pipelines made in play; FPS and playtime; off), the FPS graph under it, the
  frame rate (60 fps, the game's own pace: fixed) and MetalFX frame interpolation (not available in this game yet).
- **Input**: the controller type (auto, Xbox, PlayStation, Switch: which way the face buttons are read), rumble, and a
  DSU (cemuhook) server for a controller's buttons and motion (off by default).
- **Effects**: the picture, 1080p as the game draws it, or upscaled with MetalFX to a larger window or full screen
  instead of the plain scaling (not checked in play yet); Reset to defaults gives the plain scaling back.
- **Key bindings**: two keys or mouse buttons and the controller buttons for every Switch button and stick direction,
  as a list or drawings of the keyboard and the controller; the screenshot (P, or the controller's touchpad held 3 s,
  or its capture button) saves the game's next frame as a PNG, in a folder the 截图分享 page chooses. The settings
  panel's own key is always 1.
- **性能排行榜** (performance board) and **截图分享** (screenshot sharing) at mac-native-games.com, under the game id
  `eow`: both off by default and never asked; nothing is sent unless the player turns 我要上榜 or 分享我的截图 on.

Not in this game's panel: the User page (the saves keep the one account they have always had, `profile/save/<id>`; the
game has its own save files in it), shader contributions (the shader list grows from a work folder's cache, see
Shaders), VR, the game's language (it follows the Mac), aspect ratio and render resolution, gyro aiming. The old keys
2-4 are gone: none of them did anything in this game.

## Keys

The default keyboard layout is the fixed one this game had before (the settings panel's key bindings change it, kept in
`keyboard.txt`; the mouse drives nothing until a button is given an action there). Controllers use the Switch's own
layout by position (east A, south B, north X, west Y); MNG Launcher's Controls page and the key bindings change it
(`controls.txt`). A confirms and B goes back in the game's menus; its own button prompts say what each does in play.

| Switch | Keys | Controller |
|---|---|---|
| A | J | east (Xbox B, PlayStation ○) |
| B | K, Space | south (Xbox A, PlayStation ×) |
| X | I | north (Xbox Y, PlayStation △) |
| Y | U | west (Xbox X, PlayStation □) |
| L | Q | L1 / LB |
| R | E | R1 / RB |
| ZL | Option | L2 / LT |
| ZR | Shift | R2 / RT |
| L3 | Z | left stick press |
| R3 | C | right stick press |
| + | Return | Menu / Options |
| − | Tab, Esc | View / Create |
| d-pad left | F | d-pad left |
| d-pad up | T | d-pad up |
| d-pad right | H | d-pad right |
| d-pad down | G | d-pad down |
| left stick | W A S D | left stick |
| right stick | arrow keys | right stick |
| capture | P | touchpad held 3 s, or the capture button |

Key 1 opens the settings panel (on a controller L3 + R3, or the button picked in the key bindings). Scripted input
(`TOTK_INPUT_SCRIPT`, `TOTK_INPUT_FILE`) always uses this default, whatever the player's layout.

## Checks

| Check | Result |
|---|---|
| ExeFS (main, sdk, subsdk0, subsdk1, rtld, main.npdm) | FS header, hash table and every block verified |
| Merged RomFS | all six IVFC levels (350,852 data blocks), then 98,996 compressed ranges; 10,965 files, 8.0 GB; a flipped byte from either NCA is caught |
| Rewrite structure (`structck.py`) | OK for all four modules, 0 violations |
| Rewrite semantics (`equiv.py`) | 36,452 sites, 0 divergent; 1,200 of 1,200 mutation controls caught |
| `probe`, `probe-audio` | pass |
| Settings panel (`tests/test_eow_settings_panel.py`, `tests/test_eow_input.py`) | the panel built from the runtime's own frontend in AppKit without the game: six tabs, every row, restore at start, key bindings, the screenshot key and hold, the board and screenshot pages kept off the network; the default keys equal the old fixed layout |
| Prepared shaders (`make shader-seed`, `make test`) | 9,856 of 9,858 shaders, 10,089 variants (233 listed bind states); every bind state in 14 played shader caches (the work folder's and 13 test arms') served; 9,233 libraries, 0 failures. A failed translation, or a failed compile of a new seed, leaves the installed seed and libraries unchanged (checked with the real translator and Metal compiler) |
| Intro movie stop (`make test`; 36 runs) | the decoder's Stop / Reset with a frame still held answers `ST_ERROR` (55 checks under AddressSanitizer; main's `movie_host.c` reads a null frame there). In game, movie skipped or played to its end, with and without the seed: main crashed at `main+0x189714c` in 1 of 12 runs, 24 runs with the fix reached the load marker with no crash observed |
| Played (Mac, M4 Pro, macOS 27.0.1) | title, prologue dungeon; about 1.5 hours over several sessions (longest about 40 minutes). Two early sessions ended in an unanswered `nn::fs::CleanDirectoryRecursively`, answered since (not yet seen in play) |

Not done yet: iPhone / iPad, more than 60 fps (the game's logic follows its frame count), the stencil
plane in half-size depth copies, play past the prologue (untested).

Tools for continuing: `scripts/disasm.py`, `scripts/xref.py`, `scripts/callers.py`, `scripts/symbolize.py`,
`scripts/lldb/` (see the work log).
