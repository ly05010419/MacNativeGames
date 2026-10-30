# Animal Crossing: New Horizons — Native Port Bring-Up and Application Bundle

The native Apple Silicon port for *Animal Crossing: New Horizons* is verified for **1.2.1** (`v458752`, title `01006F8002326000`) using base 1.0.0 and update 1.2.1.
The project delivers a fully runnable native host (`acnh-native`), an offline Metal shader precompilation pipeline, and a self-contained, signed macOS application bundle (`Animal Crossing New Horizons.app`).

**3.0.1 is an experimental bring-up target.** Its input hashes, NSO segments,
generic rewrites, static linkage, merged RomFS and native cross-module/startup
probes have been checked. Gameplay is still being investigated; a successful
package or CPU probe does not establish a playable release. 3.0.0 and 2.x are
not pinned by this build flow.

## Experimental 3.0.1 build

Use base **1.0.0** and a **separate 3.0.1 update NSP**. The builder selects the
update profile from verified CNMT metadata. It rejects unknown code/data
identities. Combined update images, cheats and DLC are not part of this flow.

On Apple silicon with macOS 26+, the command-line tools and Homebrew libraries
`opus`, `libvpx`, `libvorbis`, and `libogg`, create the isolated environment once:

```sh
python3.14 -m venv build/acnh-3.0.1/work/venv
build/acnh-3.0.1/work/venv/bin/python -m pip install -r games/acnh/requirements.txt
build/acnh-3.0.1/work/venv/bin/python games/acnh/tools/build_hactool.py --work build/acnh-3.0.1/work
```

Prepare the original inputs once, using a new `prepared` subdirectory. The
shared-font folder must contain the extractor's six BFTTF files and manifest
(`tools/extract_shared_fonts.py --firmware ... --keys ... --output ...`):

```sh
build/acnh-3.0.1/work/venv/bin/python games/acnh/prepare.py \
  game-assets/animal-crossing-new-horizons/base/v0/acnh-base-v0-01006f8002326000.nsp \
  --update game-assets/animal-crossing-new-horizons/updates/v3.0.1/acnh-update-v3.0.1-01006f8002326800.nsp \
  --keys game-assets/shared/keys/prod.keys \
  --hactool build/acnh-3.0.1/work/tools/hactool/hactool \
  --work build/acnh-3.0.1/work/prepared
```

Compile current sources and assemble the full application. For subsequent source
updates reuse the same prepared/native work folders; `build_host.py` rechecks
the prepared inputs and regenerates the tables while retaining unchanged
headers and graphics object files. Repackage to update the actual app:

```sh
MACOSX_DEPLOYMENT_TARGET=27.0 build/acnh-3.0.1/work/venv/bin/python games/acnh/build_host.py \
  --prepared build/acnh-3.0.1/work/prepared \
  --work build/acnh-3.0.1/work/native --shared-fonts game-assets/shared/fonts
build/acnh-3.0.1/work/venv/bin/python games/acnh/tools/extract_app_icon.py \
  game-assets/animal-crossing-new-horizons/updates/v3.0.1/acnh-update-v3.0.1-01006f8002326800.nsp \
  --keys game-assets/shared/keys/prod.keys --prepared build/acnh-3.0.1/work/prepared \
  --output build/acnh-3.0.1/work/native/build/AppIcon.icns
build/acnh-3.0.1/work/venv/bin/python games/acnh/tools/prepare_shader_seed.py \
  --prepared build/acnh-3.0.1/work/prepared \
  --translator build/acnh-3.0.1/work/native/build/shader-service/totk_xlate \
  --output build/acnh-3.0.1/work/native/build/shader-seed
MACOSX_DEPLOYMENT_TARGET=27.0 build/acnh-3.0.1/work/venv/bin/python games/acnh/scripts/package_native_app.py \
  --prepared build/acnh-3.0.1/work/prepared --work build/acnh-3.0.1/work/native \
  --shared-fonts game-assets/shared/fonts \
  --output "build/acnh-3.0.1/Animal Crossing New Horizons.app"
```

The icon step makes the app icon from the update's own title icon (its Control data, checked as `prepare.py` checks
it); the packager bundles `<native work>/build/AppIcon.icns`, and without it the app has no icon. The shader seed step translates every shader the game's files carry (3.0.1: 6,470, plus 1,455 shadow-map variants;
about 35 s and 450 MB) and is reused while its inputs stay the same. The packager bundles it; the app imports it into
its shader cache at the first start and compiles it during the start-up warm-up, so a scene met for the first time no
longer stops the game to translate and compile its shaders (the cause of the frame drops measured on 2026-10-29).
Without the step the app still works and translates shaders while playing.

The 3.0.1 host targets NVN 55.15 and never installs the 1.2.1 game-address
patches. It uses a separate version data folder and does not import old saves.
The launcher keeps writable caches, verification proofs and game locks beside
the app under `Animal Crossing Data`, preserving the signed app resources.
No DMG or installation is performed by these commands.

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

`build_host.py` records the game version in `build/version-config.h`; the runtime is compiled for that version only
(`runtime/game_version.h`), and the packager refuses a runtime built for another version. A native work folder made
before 3.0.1 support has no such file and is always 1.2.1: `make` writes 1.2.1's for it, and running `build_host.py`
again on the folder writes it too. The 1.2.1 game-address patches stay 1.2.1-only. `tools/extract_app_icon.py` and
`tools/prepare_shader_seed.py` (see the 3.0.1 section) take any prepared folder; both are optional for packaging.
The repository does not ignore `build/` or `game-assets/` at its root: when you keep game files there as the 3.0.1
section does, list them in `.git/info/exclude`.

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

### 6. Controls and the settings panel
The default keyboard and mouse layout keeps the left hand on the keyboard. The mouse only clicks: Animal Crossing has
nothing to aim and no camera to turn with it.

| Action | Switch | Keys | Mouse |
|---|---|---|---|
| Move | left stick | W A S D | |
| Camera | right stick | arrow keys | |
| Talk, use the tool, confirm | A | Space | left button |
| Cancel; hold to run | B | Shift, Esc | |
| Pockets | X | Tab, I | |
| Pick up | Y | E | right button |
| NookPhone | ZL | Q | |
| Reactions | ZR | R | |
| L button | L | Z | |
| R button | R | C | |
| Left stick press | L3 | Control | |
| Right stick press | R3 | V | |
| + button | + | M, Return | |
| − button | − | Backspace | |
| Tool ring | d-pad up | 3 | wheel click |
| Put the tool away | d-pad down | 5 | |
| D-pad left | d-pad left | 2 | side button 4 |
| D-pad right | d-pad right | 4 | side button 5 |

The layout, the settings panel and the host functions behind the crash fixes are the same for 1.2.1 and 3.0.1.
Key 1, or L3 + R3 together on a controller, opens the settings panel; Esc or key 1 closes it. It has three pages:

- **Display:** the overlay (FPS, shaders and load; FPS only; off), an FPS graph, the frame rate (30 fps, the game's
  own; there is no lock to pick because the game cannot run faster), MetalFX frame interpolation (shown as not
  available: its hooks do not find this game's scene, depth, camera and UI yet), the picture (1080p, or 1080p with MetalFX upscaling for a larger window or full screen) and the game language
  (follow the Mac or one of the game's 14 languages; it applies at the next launch).
- **Input:** the controller type (auto, Xbox, PlayStation, Switch) and rumble.
- **Key bindings:** drawings of the keyboard and mouse and of the controller. Click a key or button and pick what it
  does; a list view is also available. Each device has its own reset button.

The choices are saved next to the app in `Animal Crossing Data` (`controls.txt`, `keyboard.txt` and one small file per
setting). Scripted input (`commands.txt`, `TOTK_INPUT_SCRIPT`) keeps the fixed test layout, so scripts mean the same keys
whatever the player picks. `TOTK_LANG`, `TOTK_MFX_UPSCALE` / `TOTK_MFX_OUTPUT` and `TOTK_RUMBLE` decide
before the panel's choices; the panel then shows their value greyed out.

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
