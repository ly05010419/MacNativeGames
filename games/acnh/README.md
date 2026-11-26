# Animal Crossing: New Horizons — Native Port Bring-Up and Application Bundle

The native Apple Silicon port for *Animal Crossing: New Horizons* is verified for **1.2.1** (`v458752`, title `01006F8002326000`) using base 1.0.0 and update 1.2.1.
The project delivers a fully runnable native host (`acnh-native`), an offline Metal shader precompilation pipeline, and a self-contained, signed macOS application bundle (`Animal Crossing New Horizons.app`).

**3.0.1 is an experimental bring-up target.** Its input hashes, NSO segments,
generic rewrites, static linkage, merged RomFS and native cross-module/startup
probes have been checked. Gameplay is still being investigated; a successful
package or CPU probe does not establish a playable release. 3.0.0 and 2.x are
not pinned by this build flow.

**3.0.3 is an experimental target too, and the one MNG Launcher builds.** Its `rtld`, `sdk` and `subsdk0` are byte for
byte 3.0.1's; only `main` differs (`assets/expected-v2228224.json`). On a Mac it was played into the opening, in
Simplified Chinese, at 30 and 60 FPS (2026-11-17, MacBook Air M4, macOS 26.6); not further.

## One command, or MNG Launcher

`build.py` runs every step below in order and reuses what an earlier run in the same work folder made from the same
files:

```sh
python3.14 games/acnh/build.py base-1.0.0.nsp -k prod.keys --update update-3.0.3.nsp \
  [--dlc dlc.nsp ...] [--firmware <unpacked firmware> | --shared-fonts <fonts folder of an earlier build>] \
  [--work <folder>] [-o "Animal Crossing New Horizons.app"]
```

It makes its own Python (`<work>/venv`) and hactool, verifies and prepares the game data, the fonts and the add-ons,
fetches the pinned [tmouh/acnh-60fps](https://github.com/tmouh/acnh-60fps) checkout into the work folder for the frame
rate options (`--fps-mod <checkout>` uses yours, `--no-fps-mod` keeps 30 FPS only), and packages the app, which records
its work folder. Every step logs to `<work>/logs/`. MNG Launcher builds and updates the game the same way: drop the
base, the update, any add-ons and a fonts or firmware folder on its Animal Crossing card. Its update (no game files)
rebuilds the host and the app from the work folder (`update_runtime.py` calls `build.py`'s `update`). What `build.py`,
the packager and the app launcher share with the other ports MNG Launcher builds this way is `tools/port_app/` and
`shared/app/app_launcher.h` (shared/README.md).

The app's bundle ID is `org.macnativegames.acnh`, signed ad hoc with the bundle ID as its designated requirement, so
macOS keeps the "files on a removable volume" answer across builds. The player's data lives in
`~/Library/Application Support/MacNativeGames/ACNH-<version>` (`ACNH` for 1.2.1); a `Animal Crossing Data` folder next to
an older app is moved there at the first start.

### 120 FPS (3.0.3)

A 3.0.3 build with the frame-rate data also offers **120 FPS (experimental)** in the settings panel, from the mod's
120 FPS files (7,068 code words and its parameter pack). It is offered only while a 120 Hz screen is connected: the
host shows one picture per refresh, so on a 60 Hz screen the game would get 60 updates a second with every speed
corrected for 120 and play at half speed (measured). A saved 120 then starts at 60 and says so in the log. 120 FPS was
not played on a 120 Hz screen yet.

## Experimental 3.0.1 build

Use base **1.0.0** and a **separate 3.0.1 update NSP**. The builder selects the
update profile from verified CNMT metadata. It rejects unknown code/data
identities. Combined update images and cheats are not part of this flow. The three
separate, pinned DLC packages can optionally be added as described below.

On Apple silicon with macOS 26+ and the command-line tools (opus, libvpx, libvorbis and libogg come with the build:
pinned sources, made once per Mac by `tools/deps`; no Homebrew), create the isolated environment once:

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

### Optional DLCs for 3.0.1

Three DLCs are pinned: **Pocket Camp Special Order Ticket** (`01006F800232712C`,
index 300), **Nook Inc. Silk Rug** (`01006F800232712D`, index 301), and **Happy Home
Paradise** (`01006F80023273E8`, index 1000), all version 0. Supply any of the owner's
separate NSPs (one `--source` each); the others are simply absent. No game content is
distributed with the source. The preparer checks
their exact package hashes, AddOnContent CNMT type/version/application identity,
FS headers and all six IVFC levels against `assets/expected-dlc-v0.json`.

```sh
build/acnh-3.0.1/work/venv/bin/python -B games/acnh/tools/prepare_dlc.py \
  --source game-assets/animal-crossing-new-horizons/dlc/v0/acnh-dlc-pocket-camp-special-order-ticket-v0-01006f800232712c.nsp \
  --source game-assets/animal-crossing-new-horizons/dlc/v0/acnh-dlc-nook-inc-silk-rug-v0-01006f800232712d.nsp \
  --source game-assets/animal-crossing-new-horizons/dlc/v0/acnh-dlc-happy-home-paradise-v0-01006f80023273e8.nsp \
  --keys game-assets/shared/keys/prod.keys --output build/acnh-3.0.1/work/dlc
```

Reuse this verified output on subsequent builds. Add `--dlc build/acnh-3.0.1/work/dlc`
to the `build_host.py` command above, then repackage. The native work folder remembers
the DLC path; `--no-dlc` forgets it. Builds in a new work folder without this option
report zero DLCs. The packager refuses a runtime built for a different DLC manifest and
bundles every prepared read-only image. The runtime checks each image's full hash, keeps
a verification record under `Animal Crossing Data` and reuses it while the image file is
unchanged (as for the base RomFS; `TOTK_ROMFS_VERIFY=full` forces a new hash), enumerates
the actual DLC indices and serves file slices from each mounted image. A missing or
changed image stops the launch rather than hiding content a save may need. The base/update RomFS
and existing saves remain separate from the DLC images.

To check the packaged runtime without launching gameplay:

```sh
ACNH_DLC_DIR="$PWD/build/acnh-3.0.1/Animal Crossing New Horizons.app/Contents/Resources/game/dlc" \
  "build/acnh-3.0.1/Animal Crossing New Horizons.app/Contents/MacOS/acnh-native" \
  "build/acnh-3.0.1/Animal Crossing New Horizons.app/Contents/Resources/game/rewritten" probe-dlc
```

With all three DLCs this probe checks all 1,173 DLC file slices, guest-facing enumeration/mount/cache
queries, size/read/EOF behavior, write rejection and traversal rejection. It does
not establish that Happy Home Paradise or the promotional rewards work in gameplay;
3.0.1 and its DLC support remain experimental.

### Optional 30 / 60 FPS selection for 3.0.1

30 FPS remains the default. A build with the optional speed-correction inputs
offers **30 FPS** and **60 FPS (experimental)** under **Settings > Display**
(key 1). The choice is saved as `Animal Crossing Data/frame-rate.txt` and applies
after quitting and restarting the game. Code and parameter corrections must be
loaded together; selecting 30 restores the original game timing on the next
launch. No other cheats are enabled, and MetalFX frame interpolation remains
unavailable.

The local 60 FPS inputs are [tmouh's ACNH 60 FPS + SpeedFix](https://github.com/tmouh/acnh-60fps)
at commit `e5e03e64739334c9fbd36f19658b98e805f8ba8d`, under **CC BY-NC 4.0**.
Keep the source checkout outside tracked code, for example under
`game-assets/animal-crossing-new-horizons/mods/v3.0.1/acnh-60fps-source`.
Its code/parameter data is not redistributed in this repository. No external
mod tools or emulator code are executed or imported by the build.

Add this option to the 3.0.1 `build_host.py` command above, then repackage:

```sh
--fps-mod game-assets/animal-crossing-new-horizons/mods/v3.0.1/acnh-60fps-source
```

The work folder remembers this input for subsequent rebuilds; `--no-fps-mod` forgets it. The builder checks
the six source-file hashes, the original NSO and RomFS identities and all 6,417
patch sites against the original and rewritten main module. It derives a separate
parameter overlay from the owner's files, verifies its result, and packages both
the complete speed corrections and the original license/attribution. On a 60 FPS
launch the loader rechecks every original word before changing any of them, then
seals executable memory before the game starts. Original NSOs, rewritten files,
RomFS and saves are not patched on disk. The builder also rejects any replacement
that the instruction rewriter would have gated (`svc`, system-register access, x18)
or that branches outside the code or into rewritten code. Selecting 30 FPS locks
presentation at 30; builds without these inputs, including 1.2.1, keep the game's own
presentation interval and timing.

For isolated diagnostics, `ACNH_FPS=30|60` overrides the saved choice and locks
the panel's row; a standalone native run also needs `ACNH_FPS_ASSETS` set to the
absolute `<native work>/build/fps60` directory. The packaged launcher sets the
asset path automatically. 60 FPS is a fixed-rate experimental mode: sustained
performance below its target can slow frame-counted actions. Full gameplay
coverage is still required before treating it as a verified release.

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
| Screenshot (the game's picture, saved as a PNG) | capture | P | |

The layout, the settings panel and the host functions behind the crash fixes are the same for 1.2.1 and 3.0.1.
Key 1, or L3 + R3 together on a controller, opens the settings panel; Esc or key 1 closes it. Since 2026-11-17 it is
the Zelda games' panel (`shared/runtime/settings_panel.h`, this game's rows in `runtime/settings_game.h`), with the
pages this game supports:

- **Display:** the overlay (all: FPS, shaders, load and playtime; FPS full information, which adds every long frame
  (hitch), shader libraries, pipelines and archive misses; FPS and playtime; off), an FPS graph, the frame rate (30, or
  experimental 60 fps (3.0.1, 3.0.3) and 120 fps (3.0.3) for builds with speed-correction data, applied after
  restarting; 120 is offered only while a 120 Hz screen is connected, and a saved 120 starts at 60 on a 60 Hz one;
  otherwise 30 fps), MetalFX frame interpolation (shown as not available: its hooks do not find this game's scene,
  depth, camera and UI yet) and the game language (follow the Mac or one of the game's 14 languages; it applies at the
  next launch). The window opens at the game's 1920x1080 picture, pixel for pixel.
- **Input:** the controller type (auto, Xbox, PlayStation, Switch), rumble, and DSU (cemuhook) input over UDP: a switch,
  the server, the slot and its status (off by default).
- **Effects:** the picture (1080p, or 1080p with MetalFX upscaling for a larger window or full screen), and **Reset to
  defaults** (plain scaling).
- **Key bindings:** drawings of the keyboard and mouse and of the controller. Click a key or button and pick what it
  does; a list view is also available. Each device has its own reset button. The last row is the screenshot: P, or
  on a controller the touchpad held for 3 seconds (any button can take the hold); the controller's capture or Share
  button takes one too. A screenshot is the game's next frame without the panel or the corner box, saved as a PNG on
  the Desktop or in the folder chosen on the 截图分享 page.
- **性能排行榜** (Performance) and **截图分享** (Screenshots): the frame-rate board and the shared screenshots of this game
  (`acnh` at mac-native-games.com), with the player's nickname. Their switches 我要上榜 and 分享我的截图 are off by default;
  nothing is sent unless the player turns them on.
- **User:** the local users (shared/runtime/users.h), as in the Zelda games: the page shows the user this run opened
  with, a popup chooses the user the next start plays as, and **添加用户…** adds one (the same full-window text keyboard
  as the Zelda games). Each user gets a save folder of its own (`profile/save/<user id>`), so a second user starts a
  second island — a console shares one island between its users instead. An existing save keeps the account it had (the
  user "Local" when the profile predates the users; this profile's first user is "duanxinyi").

Left out: the shader contribution page (this game's shaders have no shared list to contribute to), VR, rumble strength,
gyro aiming and mouse sensitivity. The corner box is the Zelda games' too: FPS, the other lines the overlay choice shows,
the session's and the total playtime (`profile/playtime.json`), the controller's battery and how to open the panel.

The choices are saved in the app's data folder (`controls.txt`, `keyboard.txt` and one small file per setting,
including `frame-rate.txt`). Scripted input (`commands.txt`, `TOTK_INPUT_SCRIPT`) keeps the fixed test layout, so scripts mean the same keys
whatever the player picks. `TOTK_LANG`, `TOTK_MFX_UPSCALE` / `TOTK_MFX_OUTPUT` and `TOTK_RUMBLE` decide
before the panel's choices; the panel then shows their value greyed out. `ACNH_FPS` likewise takes priority over the frame-rate choice.

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
