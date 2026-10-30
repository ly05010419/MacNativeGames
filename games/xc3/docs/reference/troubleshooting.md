# Troubleshooting

Symptom → cause → fix, added as problems are solved. Search this file before debugging something that looks familiar.

| Symptom | Cause | Fix | Stage / commit |
|---|---|---|---|
| Crash at `main+0xd27ddc` (null + 0x50) a few frames after the start; `rom-not-found rom:/bdat/us/…` just before | the language was `en-US`, which 1.0.0 does not support (only en-GB), so a resource failed to load | language picked among control.nacp's supported ones (`GAME_SUPPORTED_LANGUAGES`) | stage 5 |
| `sm: service "X" not implemented` | an SDK function reached IPC the host does not answer | find it with `grep caller log \| XC3_WORK=$WORK $PY scripts/symbolize.py`, hook the function | stage 4 |
| Crash inside an `nn::*` SDK function at a small address | the SDK's service object is null (no IPC session) | same: hook the function | stage 5 |
| A still picture at 30 fps | the game waits for a button | `TOTK_INPUT_SCRIPT="40:38:150"` presses A (J) | stage 5 |
| Exit 142 | the 2-minute alarm of the boot script, not a crash | — | — |
| `make boot` exits 1 with `VI requires native macOS window` | the target always set `TOTK_HEADLESS` (empty), and the runtime goes headless when the variable exists | fixed 2026-10-29: the target no longer sets it | stage 6 |
| `make boot` shows "Warming shaders 100%" forever; the log says `GPU submission disabled` | the shared layer submits no GPU work unless `TOTK_GPU_ALLOW_SUBMIT=1` (the app sets it, the target did not) | fixed 2026-10-29: the target sets it by default | stage 6 |
| Cutscenes enlarged and shifted to the top-left until restart | key 2 had turned on MetalFX frame interpolation; interpolated frames ignore the game's 1280×720 crop | fixed 2026-10-29: keys 2–4 are off in `runtime/input.c` | stage 7 |

## Before adding a workaround
- Is the check that failed correct? Never weaken hash, structure or equivalence checks to get past an error.
- Is it a platform-layer bug (ABI, TLS, memory, threads, files, NVN)? Fix it there, not in game-specific code.
- Has another port already met it? Check the root `docs/` and the other ports' `CHANGELOG.md`, then copy the fix and record it in `third_party/COPIES.md`.
