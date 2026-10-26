# Mario vs. Donkey Kong (early port)

Mario vs. Donkey Kong 1.0.1 (`0100B99019412000`, base game + the 1.0.1 update) running natively on Apple silicon Macs, built
from **your own copy** of the game. The game runs on the same Nintendo engine as Super Mario Bros. Wonder (sead, `.bgyml`,
RSDB, zstd-packed resources), so this folder is a fork of `games/wonder` and keeps its own copies of everything
(`runtime/`, `third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`, `scripts/`): changes here never affect another game.

**No game content is in this repository.** You need a legal dump of your own game and keys from your own Switch.

## Build

Needs a Mac with Apple silicon and macOS 26 or later, the Xcode command line tools, Python 3.14, and from Homebrew
`opus libvpx`.

```sh
cd games/mvdk
python3.14 scripts/build_mvdk.py --game "<your base game>.nsp" --update "<your 1.0.1 update>.nsp" --keys "<your prod.keys>" \
    --firmware "<unpacked firmware NCA folder>"
```

- The game file is the base game, NSP or XCI; the update is 1.0.1 (`0100B99019412800`, v65536), and the app always runs
  1.0.1: its code from the update, its RomFS the base game's merged with the update's (all six IVFC levels verified).
  Every step is checked against `assets/expected-v1.0.1.json` (hashes only).
- An NSP carries no system fonts: pass `--firmware`, or `--shared-fonts <folder>` with a font folder an earlier build
  verified (`extracted/firmware/shared-fonts` in any of this repository's work folders).
- The work folder is `MvDK_build` next to the game file (`--work` to choose another). It holds game data: never share or
  commit it. The app `Mario vs. Donkey Kong.app` goes next to the game file (`--out` to choose).
- Saves, the shader cache and the log are in `~/Library/Application Support/MacNativeGames/MvDK` and
  `~/Library/Logs/MacNativeGames/mvdk.log`; a build never touches them.

## What changed from games/wonder

| File | Change |
| --- | --- |
| `runtime/game_version.h` | a `GAME_MVDK` block: title ID, name, data folder `MvDK`, log `mvdk`, version folder `mvdk101` |
| `Makefile` | `-DGAME_MVDK=1 -DGAME_WONDER=1` (Wonder's offline services and 60 Hz VI vsync apply), binary `build/mvdk-native` |
| `runtime/fs_host.c`, `runtime/services.c` | the game's application / program ID |
| `runtime/game_patches_mvdk.c` | replaces `game_patches_wonder.c`: exclusive loads with acquire (sead spin locks), no game-code sites; held at 60 fps |
| `scripts/build_mvdk.py` | Wonder's build with three modules (`main`, `subsdk0`, `sdk`), the update required |
| `scripts/merge_romfs.py`, `prepare_runtime.py`, `prepare_romfs_backend.py` | the title ID and the `mvdk101` version |
| `runtime/movie_host.c` | DKCR's (H.264 through VideoToolbox) plus AAC (decoder type 4) through AudioToolbox, its output buffers exactly one unit long (the game copies a whole buffer: a longer one left silence after every unit), and H.264 texture mode (mode 2, RGBA) for the title-screen movies |
| `runtime/audio_out.c` | from games/dkcr: `audout:u`, the movie sound's output; `TOTK_AUDIOOUT_TRACE=1` logs the queue each second, `TOTK_AUDIOOUT_DUMP=<file>` keeps the PCM |
| `third_party/d2r_gfx/gfx/nvn/hle_nvn.c` | `dm_nvn_texture_write_rgba` (from games/ori); advanced blend modes (Porter-Duff) as ordinary blend factors |
| `Makefile` | also links VideoToolbox and CoreMedia |
| `runtime/gamepad.m`, `runtime/input.c` | two players: the first controller (with the keyboard and mouse) is player 1, the second is the Switch's controller No. 2, with the same layout file; the controllers' lights show 1 and 2. `TOTK_TWO_PLAYERS=0` merges every controller into player 1 |

The game imports the runtime GLSL compiler (`glslc*`); those symbols are weak and resolve to zero, as on the Switch when it
is not linked. `subsdk0` is the same audio/movie middleware Donkey Kong Country Returns HD ships.

## Status

Tested on a Mac (October 2026) with scripted input: the intro cutscene with sound, the title screen, a new save, the
opening scene and Level 1-1 at a steady 60 fps with its HUD, the pause menu, two-player co-op with two controllers; the intro's sound is continuous (checked
against a reference decode of the same track). Not yet: later levels, saves across restarts, a
startup shader set, the advanced blend modes that need the destination in the shader (multiply, screen, overlay, ...).
