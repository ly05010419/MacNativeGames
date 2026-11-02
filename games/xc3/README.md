# Xenoblade Chronicles 3 (work in progress)

Xenoblade Chronicles 3 running natively on Apple silicon Macs, the way `games/totk` runs Tears of the Kingdom:
- the game's own ARM64 code is rewritten ahead of time and runs directly on the Mac's CPU,
- Horizon's services are answered by a native host,
- NVN graphics are replayed on Metal 4, with the game's shaders translated to Metal.

This folder started as a copy of `games/ot0` (Octopath Traveler 0), itself a copy of `games/by3` and `games/totk`. It keeps its own copies of everything it builds and runs with: `runtime/`, `third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`, `scripts/`, and the shader translator's source in `tools/totk_xlate`. Nothing here reads another game's folder; [third_party/COPIES.md](third_party/COPIES.md) says where each copy came from. How the port was done and what was found on the way: [CHANGELOG.md](CHANGELOG.md). Technical notes (transform feedback, geometry shaders, NVN coverage, services, game facts) are in [docs/reference](docs/reference/).

You must own the game: use your own NSP and the `prod.keys` and system fonts dumped from your own Switch. Nothing from the game is in this repository. `XC3_build/` (the work folder) and `mods/` are ignored by git.

## Status (29 October 2026)

Tested on one Mac: Apple M5, 32 GB, macOS 27.0.1, game versions 1.0.0 and 2.2.1.

| Stage | State |
|---|---|
| Code and data | done: 1.0.0 code and RomFS extracted from the base NSP and verified (ExeFS hashes, all six RomFS IVFC levels) |
| Rewrite | done: sdk, subsdk0 and main rewritten and verified (`structck.py`, and `equiv.py --min-eff 8` with 0 divergent sites) |
| Boot | done: SDK startup, every constructor (main 805, subsdk0 45, sdk 112), `nnMain`, the game's threads, window and Metal device |
| Graphics | mostly done: transform feedback and geometry shaders (point input) are emulated, GPU BC7 encoding, several texture pools, vertex-shader skinning |
| Menus and new game | done: difficulty selection, both prologue movies (WebM through `runtime/movie_host.c`), the opening cutscene |
| Battles | done: the first battle, with characters, and later ones played by hand |
| Sound | done: Wwise through the audio renderer; checked by ear |
| Save and load | done: saved in Everblight Plain, quit, started again and loaded at the same place |
| Frame rate | 30 fps, as on the Switch; the GPU takes about 6–7 ms per frame in Everblight Plain |
| App | done: `make app` builds a self-contained `Xenoblade Chronicles 3.app` (settings window, then the game); it runs with the work folder renamed |
| 2.2.1 | builds from the base NSP plus the 2.2.1 update; played on the Mac from a 1.0.0 save (29 October 2026); the DLC menu's "Purchase" returns at once (no eShop) |

## Known gaps

- Geometry shaders are emulated for point input only. A program with triangle-strip input is skipped (logged once).
- Logic op OR on integer render targets is not emulated.
- One `DrawTransformFeedback` with nothing captured, near the second prologue movie (logged once).
- Under the game's second sampler pool, a combined handle with sampler 0 is read as a separate texture handle.
- Rare, brief screen tearing or thin black lines while playing.
- Now and then a long, thin bright streak across the scene from a character (seen in play at the original scale, and once in an agent run at 2K).
- A shader seen for the first time is translated and compiled when it is first used. On the first run (of `make boot` or of the app, which keeps its own cache) a loading screen can wait for that, once for up to about a minute; later runs load the shaders from the cache.

## What the game uses

- **NVN:** SDK 12.3.9, the same in 1.0.0 and 2.2.1. The game asks `nvnDeviceGetProcAddress` for entry points 1,050 times at start. The graphics layer's table has 523 NVN names: 432 implemented, 7 intentional no-ops, and the other 84 abort with their name when called. None of those 84 was called in 15-minute runs of either version.
- **Features Metal lacks, emulated here:**
  - transform feedback;
  - geometry shaders (point input; one triangle-strip program is skipped);
  - BC7 encoding on the GPU;
  - two texture and sampler pools switched every frame;
  - skinning in vertex shaders under rasterizer discard;
  - quads.
  See [docs/reference/nvn-coverage.md](docs/reference/nvn-coverage.md).
- **Sound and movies:** Wwise sound through `audren:u` / `audout:u`, and 238 WebM (VP9) movies through the SDK `movie` library and `runtime/movie_host.c` (libvpx).
- **Online and DLC:** no online play. In 2.2.1 the game sees your own add-ons (Expansion Pass volumes 1–4) once they are extracted, and the eShop's "Purchase" returns at once.

## Inputs

- Base game 1.0.0: `010074F013262000` (NSP; v0). It holds the code as well as the data; the scripts identify it by its ExeFS hashes and its Program NCA against `assets/expected-v1.0.0.json` (hashes only).
- Code and SDK: `main`, `subsdk0` (the SDK multimedia library) and `sdk`, Nintendo SDK 12.3.9, NVN graphics, Wwise sound, WebM movies.
- Update 2.2.1 (v720896): `010074F013262800` (NSP). Its code is what runs; the RomFS is the base plus this update (BKTR). The scripts tell the versions apart by their contents, never by the file name: `extract_code.py` by the ExeFS hashes and `merge_romfs.py` by the update's Program NCA, against `assets/expected-v2.2.1.json`. A save written by 2.2.1 may not load in 1.0.0.
- Your own `prod.keys`, and the system shared fonts from your own console.

## Build and run

You need Xcode's Command Line Tools, Python 3.14 and, from Homebrew, `llvm` (for `structck.py`), `opus` and `libvpx`. Xcode itself and the .NET SDK are not needed: the shader translator is prebuilt in `prebuilt/` and checked by its SHA-256.

From `games/xc3`, with a Python 3.14 environment made from `requirements.txt` (`PY=<that python>`):

```sh
$PY scripts/extract_code.py "<base>.nsp" --keys prod.keys --work XC3_build    # code -> XC3_build/extracted/v100/exefs
$PY scripts/merge_romfs.py "<base>.nsp" --keys prod.keys --work XC3_build     # data -> XC3_build/extracted/v100/romfs
mkdir -p XC3_build/extracted/firmware/shared-fonts && cp <your shared fonts>/* XC3_build/extracted/firmware/shared-fonts/
for m in sdk subsdk0 main; do
  DM_RULESET=native DM_PRIVATE_GATES=0 $PY third_party/d2r/patch_nso.py XC3_build/extracted/v100/exefs/$m XC3_build/rewritten/v100 $m
done
make prepare runtime shader-tools VERSION=v100 PY=$PY
make boot VERSION=v100
```

For 2.2.1, extract the update's code and merge its RomFS with the base, then do the same with `v221`:

```sh
$PY scripts/extract_code.py "<update 2.2.1>.nsp" --keys prod.keys --work XC3_build                  # -> XC3_build/extracted/v221/exefs
$PY scripts/merge_romfs.py "<base>.nsp" "<update 2.2.1>.nsp" --keys prod.keys --work XC3_build      # -> XC3_build/extracted/v221/romfs
for m in sdk subsdk0 main; do
  DM_RULESET=native DM_PRIVATE_GATES=0 $PY third_party/d2r/patch_nso.py XC3_build/extracted/v221/exefs/$m XC3_build/rewritten/v221 $m
done
make prepare runtime shader-tools VERSION=v221 PY=$PY
make boot VERSION=v221
```

Add-on content (2.2.1 only): your own Expansion Pass NSPs, each checked against `assets/expected-aoc.json` (hashes only):

```sh
$PY scripts/extract_aoc.py "<wave 1>.nsp" "<wave 2>.nsp" "<wave 3>.nsp" "<wave 4>.nsp" --keys prod.keys --work XC3_build   # -> XC3_build/extracted/aoc/1..4
```

Both versions live in one work folder (`extracted/<version>`, `rewritten/<version>`, `build/<version>` and `build/xc3-native-<version>`); the shader cache and the tools are shared. `VERSION` defaults to the newest version extracted. `make app VERSION=v221 …` builds the app for 2.2.1.

`make boot` opens the game window and writes its log to `XC3_build/logs/boot.log`. The first start translates the game's shaders as they are used; later starts warm them from the cache in a second or two. The system shared fonts can be extracted from your own firmware with `scripts/extract_shared_fonts.py`.

Verification of the rewrite (each module): `third_party/d2r/structck.py XC3_build/rewritten/v100 <module> XC3_build/extracted/v100/exefs/<module>` and `third_party/d2r/equiv.py` with the same arguments plus `--min-eff 8`. Set `DM_ROOT` to the work folder so `structck.py` keeps its cache there. `make prepare` checks every rewritten module against `rewritten_sha256` in the manifest.

Keys as in the other ports: WASD the left stick, arrow keys the right stick (camera), J is A, K or Space is B, I is X, U is Y, Q / E are L / R, Option / Shift are ZL / ZR, Return is +, Tab or Escape is −, T F G H the D-pad. A game controller works too. Key 1 opens the settings panel (from TotK): overlay and FPS graph, controller type, mouse, rumble, and key bindings for keyboard, mouse and controller, saved for the next start. In 2.2.1, **Render resolution** draws the game's scene at the full 1920×1080, or at 2K (2552×1440, which the game scales down to its 1080p picture), instead of its own 960×540 (half the output, as the Switch does); it applies at the next start and is off by default. Rows this game cannot use yet (frame lock above 30, frame interpolation, output resolution, aspect ratio) show one fixed value. On a controller, L3 + R3 together open it.

Run this way, saves, the profile and the pseudo device id are written to `XC3_build/user-data`.

## The app

`make shader-seed PY=$PY` first (optional) gathers the shaders already translated on this Mac (the work folder's cache and the app's) into `build/shader-seed`; `make app` then bundles them, so a fresh app does not translate them while playing. `make app VERSION=v100 PY=$PY APP=<path>.app` builds the app: a launcher with a settings window and, in `Contents/Resources/game`, everything the game reads (the runtime, the shader tools, the rewritten modules, the RomFS and the system fonts), cloned from the work folder. On the work folder's APFS volume the clones take no extra space; on another volume they are copies (about 14 GB). Add `GAME=<base>.nsp KEYS=<prod.keys>` to give it the game's own icon, read from your NSP at build time. The app keeps working when the work folder is moved or deleted.

The settings window chooses the monitor, window or full screen, the window size or full-screen resolution, MetalFX or plain upscaling, the frame rate overlay, and whether new shaders are built in the background. There is no frame rate choice: the game holds itself to 30 fps, as on the Switch.

The app writes only to `~/Library/Application Support/MacNativeGames/XC3` (saves, profile, shader cache) and `~/Library/Logs/MacNativeGames/xc3.log`. Its saves are separate from those of `make boot`, which are in `XC3_build/user-data`.

## Next

- The known gaps above, starting with geometry shaders of other input topologies.
