# Octopath Traveler 0 (work in progress)

Octopath Traveler 0 running natively on Apple silicon Macs, the way `games/totk` runs Tears of the Kingdom. The game's own ARM64 code is rewritten ahead of time and runs directly on the Mac's CPU; Horizon's services are answered by a native host, and NVN graphics are replayed on Metal 4 with the game's shaders translated to Metal.

This folder started as a copy of `games/by3` (Bayonetta 3), itself a copy of `games/totk`, and keeps its own copies of everything it builds and runs with (`runtime/`, `third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`, `scripts/`, and the shader translator's source in `tools/totk_xlate`, which carries two patches more than TotK's). Nothing here reads another game's folder; [third_party/COPIES.md](third_party/COPIES.md) says where each copy came from. How the port was done and what was found on the way: [CHANGELOG.md](CHANGELOG.md).

You must own the game: use your own card image or NSPs and the `prod.keys` dumped from your own Switch. Nothing from the game is in this repository; `OT0_build/` (the work folder) and `mods/` are ignored by git.

## Status (27 October 2026)

Tested on one Mac: Apple M5 Max, 64 GB, macOS 27.

| Stage | State |
|---|---|
| Code and data | done: 1.0.3, 1.0.4 and 1.0.5 code extracted and rewritten, all three modules verified (structure and emulator equivalence), RomFS merged and verified |
| Boot | done: SDK startup, 1,453 constructors, the engine's own startup |
| Menus | done: Initial Settings (saved to the profile), logos, title screen with its CRIWARE background movie, main menu |
| New game | done: storybook intro, character creation (appearance, skill, belongings), favourite dish and name typed in the Mac text bar, story narration, the first town in HD-2D |
| Sound | done: AudioOut streams play through Core Audio; checked by ear, it plays correctly |
| Battles | 1.0.4: many battles played on the Mac, including Ultimate Techniques, whose rendering problems were found and fixed this way ([CHANGELOG.md](CHANGELOG.md)) |
| Frame rate | up to 120 fps (the Switch version is held to 30); 8.3 ms per frame in the first town on a 120 Hz display |
| 1.0.3 | played on the Mac (27 October 2026): to Atlas Dam, with a few battles just outside it |
| 1.0.5 | started on the Mac (28 October 2026): title screen, main menu and the world map from a save, at 120 fps |
| App | `scripts/build_ot0.py` (or `make app`) builds a self-contained `Octopath Traveler 0.app`: a settings window (frame rate, monitor, window or full screen, size, output resolution, MetalFX, HUD), then the game |

Known gaps: a shader the game uses for the first time is built in the background and the objects that need it appear a moment late; draws into smaller targets (shadows, lookup tables) still wait for it, a pause of up to about 150 ms the first time. Each shader is built once; later sessions load it from the cache.

## Frame rate

The app's "Frame rate" setting (up to 120, 60 or 30; 120 by default) sets the engine's frame rate cap. The game sets Unreal's `t.MaxFPS` to 30; above 30 the runtime makes `UGameEngine::GetMaxTickRate` return the chosen cap instead, and the game presents a frame at every display refresh rather than every second one. Game speed does not change: the engine measures each frame's real duration (its fixed-frame-rate option is off). The launcher passes the setting to the runtime as `OT0_MAX_FPS` (30 to 240); run without the launcher, the game keeps its own 30 unless that variable is set.

## Inputs

- Base game: the card image `01005270232F2000` (XCI; a trimmed image works). It also carries a bundled update, which is not used.
- Update 1.0.3 (v196608), 1.0.4 (v262144) or 1.0.5 (v327680): `01005270232F2800` (NSP). Its code is what the app runs; the RomFS is the base plus this update. The scripts tell the versions apart by their contents, never by the file name: `extract_code.py` by the ExeFS hashes and `merge_romfs.py` by the update's Program NCA, against `assets/expected-v1.0.3.json`, `assets/expected-v1.0.4.json` and `assets/expected-v1.0.5.json` (hashes only). An update that matches none of them stops.
- Engine and SDK, read from the code: Unreal Engine 5.4.2 on Nintendo SDK 19.3.13, NVN graphics, CRIWARE audio and movies.

## Build

You need Xcode's Command Line Tools, Python 3.14 and, from Homebrew, `opus` and `libvpx`. Xcode itself is not needed. One command builds the app from your own files:

```sh
cd games/ot0
python3.14 scripts/build_ot0.py --game "<card image>.xci" --update "<1.0.3, 1.0.4 or 1.0.5 update>.nsp" -k "<prod.keys>"
```

It makes the Python environment, extracts the update's code and tells the version by its hashes, merges and verifies the RomFS, rewrites the three modules and checks them against the pinned hashes, builds the runtime and puts `Octopath Traveler 0.app` next to the game file (`--out` chooses another folder). A second run reuses every finished step, and the logs are in `OT0_build/logs`. The first build needs internet access (Python packages and SPIRV-Cross) and about 10 GB free; the work folder's path must not contain a space, `:` or `$`, which `make` cannot handle (`--work` chooses another), and the app must go on an APFS or Mac OS Extended volume, not an exFAT card. One app holds one version: building the other update into the same folder replaces it, so give that build another `--out` to keep both.

## Building step by step

For working on the port, the same steps run one by one from `games/ot0`, with the Python environment the build made (`PY=OT0_build/venv/bin/python`). `extract_code.py` prints the version it found (`v103`, `v104` or `v105`); `V` below is that version.

```sh
$PY scripts/extract_code.py "<update>.nsp" --keys prod.keys --work OT0_build
$PY scripts/merge_romfs.py "<base>.xci" "<update>.nsp" --keys prod.keys --work OT0_build
for m in sdk subsdk0 main; do
  DM_RULESET=native DM_PRIVATE_GATES=0 $PY third_party/d2r/patch_nso.py OT0_build/extracted/$V/exefs/$m OT0_build/rewritten/$V $m
done
make prepare runtime shader-tools VERSION=$V PY=$PY
cd OT0_build && TOTK_GPU_ALLOW_SUBMIT=1 ./build/ot0-native-$V rewritten/$V boot
```

Verification of the rewrite (each module): `third_party/d2r/structck.py` (which needs Homebrew's `llvm`) and `third_party/d2r/equiv.py` with `OT0_build/rewritten/$V <module> OT0_build/extracted/$V/exefs/<module>`.

Several versions can be prepared in one work folder: each has its own `extracted/<version>`, `rewritten/<version>`, generated headers in `build/<version>` and runtime `build/ot0-native-<version>`, while the shader cache and the tools in `build/` are shared. The runtime stays directly in `build/` because the graphics layer finds the shader tools and cache next to it. `VERSION` defaults to the newest version extracted. Code patches go in `mods/<version>/<name>/exefs` (`mods/1.0.3`, `mods/1.0.4`, `mods/1.0.5`), are made into tables by `make patches` and checked against that version's build ID; the game applies the ones `OT0_PATCHES` names when it is run this way.

Keys as in the other ports: WASD the left stick, arrow keys the right stick, J is A, K or Space is B, I is X, U is Y, Q / E are L / R, Return is +. When the game asks for text, a bar opens at the bottom of the window: type, Return confirms. The mouse pointer hides after 5 seconds without moving over the game window and comes back when it moves.

Run this way, saves, the profile and the pseudo device id are written to `OT0_build/user-data`.

## The app

`make app VERSION=$V PY=$PY APP=<path>.app` builds the app for one version: the launcher with its settings window and, in `Contents/Resources/game`, everything the game reads (the runtime, the shader tools, the rewritten modules and the RomFS), cloned from the work folder. On the work folder's APFS volume the clones take no extra space; on another volume they are copies (about 4 GB). The app then runs without the work folder. The game writes only to `~/Library/Application Support/MacNativeGames/OT0` (`profile` with the saves, the same for both versions, `shader-cache`, `game-slot` and `mods`) and to `~/Library/Logs/MacNativeGames/ot0.log`, so the app's signature stays intact. Building it for the other version replaces it unless `APP=` gives another name.

## Mods

The app loads the RomFS mod folder `~/Library/Application Support/MacNativeGames/OT0/mods/<version>` (`mods/v104` for 1.0.4) when it holds a `manifest.tsv`, as the other self-contained ports do. Put the replacement files there under their RomFS paths and run `make mods VERSION=$V PY=$PY` to write the manifest; a mod made for another version stops the start instead of loading.
