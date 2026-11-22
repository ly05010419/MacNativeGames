# 明星三缺一 2013 on the Zelda-line runtime (bring-up)

The Switch release of 明星三缺一 2013 (title 01004D4A53510000) running natively on Apple silicon Macs, on the same code as
Tears of the Kingdom, Mario Kart 8 Deluxe and Red Dead Redemption: the game's ARM64 code rewritten ahead of time
(`third_party/d2r`, linked from games/totk), the shared scheduler and IPC server (`shared/runtime`), and a small Horizon
host of its own for what a libnx homebrew asks over IPC. No nxnative code is used. Where each part comes from:
[third_party/COPIES.md](third_party/COPIES.md); what was found and how each step was checked: [CHANGELOG.md](CHANGELOG.md).

The game is a libnx homebrew: an x86 emulator (Unicorn) running the original Windows game, drawing in software. There is
no GPU work: the window shows the 1280x720 picture the game queues.

**No game content is in this repository.** You need your own NSP of the game and your own `prod.keys`. The work folder
holds game data: keep it outside the checkout and never share it.

## Status (22 November 2026, Mac, Apple M4)

| Works | Evidence |
|---|---|
| Boot to the title and its menu (~20 s), 30 fps (the game's own rate) | screenshots, the frame log |
| Mode menu, class menu, character select, a game of mahjong against the three AI players; discarding by touch | screenshots |
| Sound (audout:u → Core Audio), the SD card folder (`<profile>/sdmc`), keyboard / mouse / touch | logs, `tests/` |
| One core at 23-36 % (was 94 %): a game that spins on audout is held until a buffer is played | `ps` time over 10-20 s at the title, the menus and the table; no underrun logged |
| The app: `明星三缺一.app` (`make dist`), saves in `~/Library/Application Support/MacNativeGames/mj2013` | started from the bundle with a temp data folder: title, menu, `close` |

| Not yet | |
|---|---|
| iPhone / iPad | the game JIT-compiles x86 code at run time; iOS lets no app execute memory it wrote. Ways on: Unicorn's TCI interpreter in place of its JIT (needs the homebrew rebuilt from its source), or a JIT-enabled device only for development |
| An icon, MNG Launcher card | next |

## Build and run

```sh
WORK=/absolute/path/outside/the/checkout/mj2013
PY=/path/to/a/venv/bin/python            # capstone, lz4, cryptography
make prepare WORK=$WORK PY=$PY GAME="/path/to/明星打雀三缺一.nsp" KEYS=/path/to/prod.keys   # extract, check, rewrite
make runtime WORK=$WORK
cd "$WORK" && TOTK_PROFILE_DIR=/tmp/mj2013-profile TOTK_INPUT_FILE=/tmp/mj2013-input.txt ./build/mj2013-native rewritten/v0 boot
```

The app: `make dist WORK=$WORK PY=$PY` (optional `APP=<path>.app`, `ICON=<image>`) writes
`$WORK/../mj2013-app/明星三缺一.app`, the runtime, the rewritten main and the RomFS image inside (APFS clones; checked against
`assets/expected.json`). It keeps the player's data in `~/Library/Application Support/MacNativeGames/mj2013` (`profile/sdmc`,
the card backed up before each of the last five starts in `backups/`, `last-run.log`, `commands.txt` for the input lines
below); `MJ2013_DATA_DIR=<absolute dir>` uses another folder.

`TOTK_PROFILE_DIR` keeps the SD card folder (saves) out of the player's data folder (default
`~/Library/Application Support/MacNativeGames/mj2013/profile`). `TOTK_HEADLESS=1` runs without a window, `BY3_AUDIO=0`
without sound. Lines appended to the input file drive the game: `tap <x> <y>` (game pixels, 1280x720), `touch <x> <y> <ms>`,
`drag <x0> <y0> <x1> <y1> <ms>`, `key <Mac key code> <ms>`, `shot <path.png>`, `close`, plus the diagnostics in the
CHANGELOG. In the window the mouse is the touch screen; keys are the console's keyboard and the Zelda-line controller
layout (J/K/I/U = A/B/X/Y, Return = +, arrows = d-pad, WASD = left stick).

## Tests

`make test` (or `python3 -m unittest tests.test_mj2013` from this folder; a Python with capstone and lz4, `MJ2013_PY`,
adds the rewriter and patch-encoding checks): the IPC answers end to end, the SD card mapping, the framebuffer conversion
against libnx's swizzler, hid's LIFOs against libnx's reader, the JIT's cache maintenance on dual-mapped code, the rewriter
on a libnx-shaped module, the game patches, and that the port links the shared code (no copies, no nxnative). The
repository's `tests/test_active_players.py` lists this game's front end.
