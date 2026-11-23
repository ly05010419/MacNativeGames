# Persona 5 Royal on the Zelda-line runtime

Persona 5 Royal **1.0.2** (Atlus's GFD engine with CRI middleware, SDK 13.3.2, NVN; the Asian release 01004B10157F2000,
the same main as the American 01005CA01580E000) running natively on Apple silicon with the Zelda line's code: the game's
ARM64 code rewritten ahead of time (`third_party/d2r`), a Horizon host (`runtime/`, Red Dead Redemption's runtime) and the
NVN-on-Metal graphics layer (`third_party/d2r_gfx`). No nxnative code is used (`third_party/COPIES.md`).

## Status (23 November 2026, Mac, MacBook Air M4)

| What | Seen |
|---|---|
| System data, the network prompt, the Atlus logo, the opening movie (USM, VP9), the title, the main menu | screenshots |
| New game: the opening casino movie and dialogue, the casino with the player in control | screenshots |
| The game's 30 fps in the casino: frame time p50 33.33 ms, p95 33.4 ms; GPU p50 15–17 ms (p95 about 20 ms) | `dev/runs/p3.log` |
| Memory steady over 7 minutes: about 1.72 GB resident, 1.77 GB GPU; 9 texture re-uploads in all | `dev/runs/p3.log` |
| The Mac app (`Persona 5 Royal.app`) opens in a window, its title and menus named for this game | played |

Not yet: the iPhone / iPad app (the shader seed tool `scripts/prepare_p5r_shader_seed.py` is still to be written),
battles, saving and loading over a longer session.

## Build

```
WORK=/absolute/path/outside/the/checkout/p5r
python3 scripts/build_p5r.py --work $WORK --base "<base 1.0.0>.nsp" --update "<1.0.2 update>.nsp" --keys prod.keys
```

Your own NSPs and keys; the work folder holds game data and must not be shared. `make runtime WORK=$WORK` rebuilds the
runtime after a source change.

## The Mac app

```
python3 scripts/package_app.py --work $WORK --output "<folder>/Persona 5 Royal.app" --record-work $WORK
```

The app carries the runtime, the shader translator, the rewritten modules and the RomFS image as APFS clones of the work
folder's files (`app/launcher.c` says the layout). Saves, the shader cache and the log live in
`~/Library/Application Support/MacNativeGames/P5R`. Key `1` opens the settings panel (its key map is there).

## Run a test session

From the work folder, headless and with the safeguards of `games/aocx` (one game at a time, no GPU keep-alive, a GPU
memory budget, the GPU hang guard, a graceful close that waits for the GPU):

```
cd $WORK && TOTK_HEADLESS=1 TOTK_GPU_KEEPALIVE=0 TOTK_GPU_BUDGET_MB=4500 TOTK_GPU_HANG_S=8 \
  TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 \
  TOTK_PROFILE_DIR=$WORK/dev/profile TOTK_INPUT_FILE=$WORK/dev/input.txt ./build/p5r-native rewritten/local boot
```

Lines in the input file drive it: `key 38 150` (J = A), `key 3 120` (F = d-pad left), `key 36 150` (Return = +),
`shot <file.png>`, `close`. From the title: any button, d-pad left to NEW GAME, A; + skips the opening movie.

## Tests

`python3 -m unittest discover -s games/p5r/tests` and the shared ones in `tests/`.
