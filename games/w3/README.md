# The Witcher 3: Wild Hunt on the Zelda-line runtime (bring-up)

The Witcher 3: Wild Hunt **4.04b** (CD PROJEKT RED's REDengine 3 in Saber Interactive's Switch port, NVN; the American
release 0100E67012924000, base 3.7 with the 4.04b update, Hearts of Stone and Blood and Wine in its RomFS) running natively
on Apple silicon with the Zelda line's code: the game's ARM64 code rewritten ahead of time (`third_party/d2r`), a Horizon
host (`runtime/`, Persona 5 Royal's runtime, itself Red Dead Redemption's) and the NVN-on-Metal graphics layer
(`third_party/d2r_gfx`), shaders translated by Tears of the Kingdom's translator. No nxnative code is used; nxnative has
no profile for this game.

## Status (23 November 2026, Mac, MacBook Air M4)

| What | Seen |
|---|---|
| Both modules load, the 21.4 GB RomFS verifies, the 1,758 constructors of main run | `dev/runs/` |
| The Saber Interactive logo, the title ("按下任何键继续"), the main menu with both expansions | screenshots |
| A new game: the comic intro, the opening CGI movie, the first in-engine scene with Chinese subtitles | screenshots |
| The first in-engine scene complete (room, walls, lights) at 30 fps: frame p50 33.33 ms, GPU p50 16 ms | screenshots, `dev/runs/wq.log` |

Not yet: gameplay, the Mac app, the iPhone / iPad app.

## Build

```
WORK=/absolute/path/outside/the/checkout/w3
python3 scripts/build_w3.py --work $WORK --base "<base>.nsp" --update "<4.04b update>.nsp" --keys prod.keys
```

Your own NSPs and keys; the work folder holds game data and must not be shared. `make runtime WORK=$WORK` rebuilds the
runtime after a source change.

## Run a test session

Headless and with the safeguards of `games/aocx` (one game at a time, no GPU keep-alive, a GPU memory budget, the GPU hang
guard, a close that waits for the GPU, and on a crash a wait for the GPU before exiting):

```
cd $WORK && TOTK_HEADLESS=1 TOTK_GPU_KEEPALIVE=0 TOTK_GPU_BUDGET_MB=4500 TOTK_GPU_HANG_S=8 \
  TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 \
  TOTK_PROFILE_DIR=$WORK/dev/profile TOTK_INPUT_FILE=$WORK/dev/input.txt ./build/w3-native rewritten/local boot
```

Input file lines: `key 38 150` (J = A), `key 40 150` (K = B), `shot <file.png>`, `close`. From the title: A, then B
skips the intro comic back to the main menu; A for New game, New start and the difficulty starts the game.

## Tests

`python3 -m unittest discover -s games/w3/tests` and the shared ones in `tests/`.
