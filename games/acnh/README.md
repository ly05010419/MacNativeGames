# Animal Crossing: New Horizons — Native Port Bring-Up and Application Bundle

The native Apple Silicon port for *Animal Crossing: New Horizons* is verified for **1.2.1** (`v458752`, title `01006F8002326000`) using base 1.0.0 and update 1.2.1.
The project delivers a fully runnable native host (`acnh-native`), an offline Metal shader precompilation pipeline, and a self-contained, signed macOS application bundle (`Animal Crossing New Horizons.app`).

## Verified Milestones

| Component | Status | Details |
|---|---|---|
| **Base/Update Verification** | Verified | Title `01006F8002326000`, CNMT versions 0 and 458752; hash chains verified |
| **Instruction Rewriter** | Verified | `rtld`, `main`, `subsdk0`, `sdk`; no game-specific cuts, parameter patches or observation gates in the rewriter. The runtime patches two places of `main`, each instruction checked first: one thread at a time rebuilds a graphics context's dirty list (`acnh_ctx_lock.c`, `ACNH_CTX_LOCK=0` turns it off) and null guards for ui2d's LocationAdjuster (`acnh_ui_patch.c`, `ACNH_UI_PATCH=0`) |
| **Static Loader Preflight** | Verified | 367,682 relocations across `main`, `subsdk0`, `sdk`; 129 host gates |
| **Update RomFS** | Verified | All six IVFC levels passed; 6,817,228,144 bytes and 69,268 files |
| **System Services HLE** | Verified | Stubs for `nn::friends`, `nn::bcat`, `nn::ldn`, `nn::nfp`, `nn::oe`, `nn::hid` touch screens & controller input |
| **Shared Fonts** | Verified | Six system BFTTF fonts integrated via `shared_fonts_config.h` |
| **Graphics Subsystem** | Verified | Metal renderer, mid-pass depth/stencil clears (`TOTK_GPU_MIDCLEAR=1`), 30 FPS / 60 Hz pacing |
| **Audio Subsystem** | Verified | CoreAudio stereo stream verified without underruns (`rms 0.0386 peak 0.3015`) |
| **Shader Precompilation** | Verified | 200+ translated MSL shaders precompiled to `.metallib` with cache verification |
| **Packaged Application** | Verified | Self-contained `apps/Animal Crossing New Horizons.app` with launcher and automatic save backup |

## Build Pipeline

### 1. Prepare Base and Update Data
```sh
python3 games/acnh/prepare.py /path/to/base.nsp \
  --update /path/to/update-v458752.nsp \
  --keys /path/to/prod.keys \
  --work /path/to/ACNH_build \
  --hactool /path/to/hactool
```

### 2. Build the Native Diagnostic Host
```sh
python3 games/acnh/build_host.py \
  --prepared /path/to/ACNH_build/verified-v458752 \
  --work /path/to/ACNH_build/native-v1.2.1 \
  --shared-fonts /path/to/firmware/shared-fonts
```

### 3. Precompile Metal Shaders
Translate runtime shaders offline into Metal bytecode (`.metallib`):
```sh
python3 games/acnh/scripts/precompile_cached_shaders.py \
  --cache /path/to/ACNH_build/native-v1.2.1/build/shader-cache \
  --output /path/to/ACNH_build/native-v1.2.1/build/shader-libs
```

### 4. Package macOS App Bundle
Assemble the self-contained, signed application bundle into `apps/Animal Crossing New Horizons.app`:
```sh
python3 games/acnh/scripts/package_native_app.py \
  --work /path/to/ACNH_build/native-v1.2.1 \
  --output "apps/Animal Crossing New Horizons.app"
```

The packager validates all executable identities, rewrites, RomFS integrity, shared fonts, and shader libraries, compiles the native launcher (`acnh_app_launcher.c`), generates high-resolution app icons from the game's control assets, establishes ad-hoc code signatures, and seeds `apps/Animal Crossing Data`.

### 5. Running the Game
- Launch via GUI by opening `apps/Animal Crossing New Horizons.app`.
- Or run the launcher binary directly:
```sh
"apps/Animal Crossing New Horizons.app/Contents/MacOS/Animal Crossing"
```
Interactive automation and tests can deliver commands via `apps/Animal Crossing Data/commands.txt`. Save states are automatically versioned and backed up under `apps/Animal Crossing Data/backups/`.

The small offline tools are adapted from Tyraelone's common tools at
`2f00936366640ce6a7aa84536a66a96a4747f927`; provenance and MIT licensing are
under `tools/`. Rewriting and Control RomFS verification use this checkout's
existing d2r and TOTK tooling. No separate fork is required to run preparation.
The shader translator (`totk_xlate`) is carried in `games/acnh/prebuilt/macos-arm64/` with its `totk_xlate.sha256`:
it is the same NativeAOT build of this repository's Ryujinx-based translator (`games/totk/tools/totk_xlate`) that
`games/wonder` carries, byte for byte. `build_host.py` and `scripts/package_native_app.py` copy it from
there into `build/shader-service/totk_xlate` and refuse a file whose SHA-256 differs; nothing is downloaded or taken
from outside this folder. SPIRV-Cross is compiled into it, so `build/spirv-cross/spirv-cross` is bundled only if a work
folder has one (`TOTK_XLATE_SPIRV_CROSS=process`). See `prebuilt/macos-arm64/README.md`.
