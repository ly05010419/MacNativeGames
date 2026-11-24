# Dark Souls Remastered on MacNativeGames

Dark Souls Remastered **1.0.2** (FromSoftware / Virtuos, SDK 4.5.0, NVN; Title ID `01004AB00A260000`) running natively on Apple silicon: the game's ARM64 code rewritten ahead of time (`third_party/d2r`), a Horizon host (`runtime/`), and the NVN-on-Metal graphics layer (`third_party/d2r_gfx`).

## Features & Status (Mac, Apple Silicon)

| What | Status |
|---|---|
| Boot, opening logos, 1080p H.264 + AAC opening cutscene, title screen, main menu | Verified |
| Character creation (inline keyboard naming, profanity filter), saving & loading (`DRAKS0005.sl2`) | Verified |
| In-game 1920×1080 rendering: Northern Undead Asylum cell, corridors, torches & fire particles, Hollows & combat, Stray Demon hall, pause/inventory UI | Verified |
| Per-`NVNsamplerPool` sampler tables (`OP_SET_SAMP_POOL`), fixed-function alpha test (`nvnColorStateSetAlphaTest` / `nvnCommandBufferSetAlphaRef` via dynamic `[[buffer(30)]]`), MSAA resolve (`nvnCommandBufferDownsample`), `nvnCommandBufferReportValue` | Verified |
| **60 FPS** (`runtime/game_patches.c`: verified 2-instruction patch for 1.0.2; `TOTK_FPS60=0` restores 30 FPS) | Verified (p50 16.6–16.7 ms, GPU p50 6–8 ms) |
| **Async shader & pipeline compilation** (`TOTK_SHADER_ASYNC=1` in `app/launcher.c`, including async alpha-test shader compilation) | Verified |
| macOS app bundle (`Dark Souls Remastered.app`) with extracted game icon and save/cache data folder | Verified |

## Build

Provide your own legally dumped base game (`v0` XCI/NSP), `1.0.2` update NSP, and keys (`prod.keys`). The work folder holds game data and must stay outside the repository checkout and never be shared.

```bash
WORK=/absolute/path/outside/the/checkout/DSR_build

# 1. Extract, rewrite, and build the runtime + shader translator
python3 games/dsr/scripts/build_dsr.py \
  --work "$WORK" \
  --base "/path/to/DARK SOULS REMASTERED [v0].xci" \
  --update "/path/to/DARK SOULS REMASTERED [1.0.2].nsp" \
  --keys "/path/to/prod.keys"

# Rebuild runtime after C/ObjC source edits:
make -C games/dsr -j8 runtime WORK="$WORK" PY="$WORK/venv/bin/python"
```

## Package macOS App (`Dark Souls Remastered.app`)

```bash
# Optional: extract the game's icon into $WORK/build/AppIcon.icns
(cd games/pla/scripts && "$WORK/venv/bin/python" extract_app_icon.py \
  "/path/to/DARK SOULS REMASTERED [v0].xci" \
  --keys "/path/to/prod.keys" \
  --title 01004ab00a260000 \
  --output "$WORK/build/AppIcon.icns")

# Package and sign the app bundle (uses APFS zero-copy clones on the same volume)
"$WORK/venv/bin/python" games/dsr/scripts/package_app.py \
  --work "$WORK" \
  --output "/Applications/Dark Souls Remastered.app" \
  --record-work "$WORK"
```

Saves, the shader cache, and logs live in `~/Library/Application Support/MacNativeGames/DSR` (override with `DSR_DATA_DIR`).

## Switches

- `TOTK_FPS60=0`: disable the 1.0.2 60 FPS patch and run at the shipping 30 FPS.
- `TOTK_SHADER_ASYNC=0`: disable asynchronous background shader/pipeline compilation (enabled by default in `Dark Souls Remastered.app` to prevent gameplay hitches when new shaders appear).
- `TOTK_HEADLESS=1`: run without opening a window (useful for automated regression tests with `TOTK_INPUT_FILE`).

## Run a Headless Test Session

```bash
cd "$WORK" && TOTK_HEADLESS=1 TOTK_GPU_KEEPALIVE=0 TOTK_GPU_BUDGET_MB=4500 TOTK_GPU_HANG_S=8 \
  TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 \
  TOTK_SHADER_ASYNC=1 TOTK_PROFILE_DIR="$WORK/dev/profile" TOTK_INPUT_FILE="$WORK/dev/input.txt" \
  ./build/dsr-native rewritten/local boot
```

Input file commands: `key 38 200` (J = A / Back), `key 40 200` (K = B / Confirm), `key 36 200` (Return = Plus / Pause), `key 13 <ms>` (W = Forward), `shot <file.png>`, `close`.
