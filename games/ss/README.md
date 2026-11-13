# The Legend of Zelda: Skyward Sword HD (Mac, experimental)

A native Mac build of Skyward Sword HD 1.0.1 (`01002DA013484000`, v65536) from the player's own files. The
target layer (extraction, checks, packaging, settings window, corner box, shader list export) is a copy of the
Link's Awakening target; the runtime is nxnative's at `b1dee89` with this repository's own runtime changes
merged in, and the game profile is nxnative's ([provenance](third_party/COPIES.md)). Experimental: checked
from a new file through the opening with scripted input and by hand; not played through on the Mac.

## Inputs

The base game (NSP or cartridge dump) and update 1.0.1 (v65536), with `prod.keys`. `scripts/extract_ss.py`
chooses the programs by their content metas, checks header signatures, each program's SHA-256 and every
RomFS file through the update's hash tree, and writes `ExeFS/` and `RomFS/` to a new folder. The build then
checks that folder against `assets/expected-1.0.1.json`: the identity nxnative's profile gives (`external`:
the four module build IDs and the 2,470 shader keys of its map) and the files of the first checked extraction
(`observed`), which say only that a later build reads the same files. All code comes from the update; the
base game supplies most of the RomFS. No container needs an integrity exception and no shader is excepted.

## Build

Apple silicon, macOS 26 or later, Xcode with its Metal toolchain, Python 3 with `lz4`, `capstone`,
`fonttools` (and `backports.zstd` before 3.14), and `make`. No Homebrew: zstd, SPIRV-Cross, the .NET SDK 10,
libvpx and dav1d come from MacNativeGames' `tools/deps` (`tools/deps/pins.py`), made once per Mac from pinned
sources (`scripts/ss_env.py`). The shader translator is built from `third_party/nxnative` at packaging
(`scripts/build_shader_tools.py`; NuGet restores its packages once, which needs network access).

```sh
python3 scripts/extract_ss.py --base "<base>.nsp" --update "<update 1.0.1>.nsp" --keys prod.keys \
  --libhac libhac.0.19.0.nupkg --work-dir "<work>" --output "<extracted>" --evidence "<evidence>"
python3 scripts/build_ss.py "<extracted>" --work-dir "<work>" --output "<new>.app"
```

Work, output and evidence stay outside the checkout. The shaders compile only with the Metal compiler
`runtime/gfx/tools/dmsh_chain.lock` records (Xcode 27.2's, 32023.922): another one stops the build before the
shader step, with installation instructions. Saves and logs go to `~/Library/Application Support/The Legend of
Zelda Skyward Sword HD/`.

## Playing

Keyboard: WASD left stick, arrow keys D-pad, J / K / U / I for A / B / X / Y, Q / E for L / R, Z / C for ZL / ZR,
Return for +, Tab for −; or a game controller. The game's button-only controls (no motion) are used.

- **1** shows or hides the corner box; **2–6** add or remove its lines: FPS, the shaders and pipelines built on
  demand (yellow while their count rises), frame timing, an FPS graph (0–60) and the shortcut list.
- **F1** or **⌘,** opens the settings window: output size cap, full screen, controller layout, the same
  statistics, and the shader list export.

## Sharing shaders

While you play, the app keeps the shaders and pipelines it had to build on the spot in `pso_user.tsv` beside
the save folder, and builds them ahead at the next start. Settings → Shader list → **Export shader list…**
saves the ones the app's own list lacks to a file you choose (keys only, no game data). Nothing is sent: send
that file to the maintainer by hand. The maintainer merges contributions into the bundled list
(below) with `scripts/pso_seed.py`:

```sh
python3 scripts/pso_seed.py --output new_pso_manifest.tsv --shader-map "<work>/source/dm/games/01002DA013484000/shader_map.tsv" --kinds RC \
  runtime/games/01002DA013484000/pso_manifest.tsv ss-shaders-*.tsv
```

`--shader-map` refuses keys that are not shaders of this game's 1.0.1 build.

## Pipeline warm-up list

`runtime/games/01002DA013484000/pso_manifest.tsv` lists the 137 render pipelines the opening route builds (file list,
new game, the opening in Skyloft), recorded twice with `DM_PSO_MANIFEST=off` (both recordings gave the same 137, in the
same order) and merged by `scripts/pso_seed.py --kinds RC`: 142 lines with its header, every key one of the 1.0.1
build's shaders. It holds no S rows: the game creates a program for every one of its 2,470 shaders while it loads, and
the host loads each library in the background then (`mtl_prog_prefetch`), which is what an S row would warm; with them
the list would pass the 1,500-line limit (2,611 lines). The host builds the listed pipelines before the first frame.
On the same build and route (2026-11-13) the opening built 137 and 131 pipelines on demand with an empty list and 0
with this one given explicitly, and 0 again from the packaged app with the list bundled; it takes 0.33-0.35 s to
prepare ([docs/validation.md](docs/validation.md)). Places past the opening still build their pipelines on first use,
and players' `pso_user.tsv` keeps them for the next start.

## State and work to do

Checked on an M5 Pro Mac: macOS 26.5 on 2026-11-11, and macOS 27.2.1 with Xcode 27.2 on 2026-11-13 (the build with the
locked compilers, the warm-up list, the packaged app); the numbers are in [docs/validation.md](docs/validation.md).

What nxnative's profile needed and this runtime carries: dual-source blending for the 432 pixel shaders of
`dual_want.tsv` (Link's eyes, the Sailcloth), logic operations on normalized targets (the distance fog of
`Dof_FogToAlpha`; without it a brown cast and dark silhouettes), the gzip-wrapped shader archive
(`Shaders/Shaders.arc.gz`, 2,412 of the 2,470 shaders) and the encrypted `.vert` / `.frag` files left aside.

Known limits:

- Stutter the first time a place past the opening needs a pipeline: the bundled list covers the opening route
  only (see Pipeline warm-up list).
- 60 fps, as the game runs on the Switch; no frame-rate control.

Open:

- Not checked: a play-through past the opening, combat, flight, the dungeons.
- Packaging gaps, as in `games/la` and `games/aoc`: the shader libraries stay in `Resources/game/shader-cache`
  instead of a signed `Contents/Helpers/ShaderData-*.bundle`, no pipeline archive is built at packaging, and
  repackaging refuses an existing `.app`.
- The iOS subtests of the runtime's tests need the iPhone SDK, which the Mac that checked this target lacks.
