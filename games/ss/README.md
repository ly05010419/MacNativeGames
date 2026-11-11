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

Apple silicon, macOS 26 or later, Xcode with its Metal toolchain, .NET SDK 10, Python 3 with `lz4`,
`capstone`, `fonttools` (and `backports.zstd` before 3.14), `zstd` and `make`. SPIRV-Cross is the pinned
build in `prebuilt/macos-arm64/spirv-cross`, not one from `PATH`.

```sh
python3 scripts/extract_ss.py --base "<base>.nsp" --update "<update 1.0.1>.nsp" --keys prod.keys \
  --libhac libhac.0.19.0.nupkg --work-dir "<work>" --output "<extracted>" --evidence "<evidence>"
python3 scripts/build_ss.py "<extracted>" --work-dir "<work>" --output "<new>.app"
```

Work, output and evidence stay outside the checkout. The shaders compile with the Mac's own Metal compiler
(`DM_ALLOW_OTHER_METAL=1` in `scripts/ss_env.py`): `dmsh_chain.lock` records Xcode 27.2's, and the build log
names the one used. Saves and logs go to `~/Library/Application Support/The Legend of Zelda Skyward Sword HD/`.

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
that file to the maintainer by hand. The bundled list is still nxnative's provisional one (empty); the
maintainer merges contributions with `scripts/pso_seed.py`:

```sh
python3 scripts/pso_seed.py --output new_pso_manifest.tsv --shader-map "<work>/source/dm/games/01002DA013484000/shader_map.tsv" \
  runtime/games/01002DA013484000/pso_manifest.tsv ss-shaders-*.tsv
```

`--shader-map` refuses keys that are not shaders of this game's 1.0.1 build.

## State and work to do

Checked on an M5 Pro Mac (macOS 26.5) on 2026-11-11; the numbers are in [docs/validation.md](docs/validation.md).

What nxnative's profile needed and this runtime carries: dual-source blending for the 432 pixel shaders of
`dual_want.tsv` (Link's eyes, the Sailcloth), logic operations on normalized targets (the distance fog of
`Dof_FogToAlpha`; without it a brown cast and dark silhouettes), the gzip-wrapped shader archive
(`Shaders/Shaders.arc.gz`, 2,412 of the 2,470 shaders) and the encrypted `.vert` / `.frag` files left aside.

Known limits:

- Stutter the first time a place needs a pipeline: no warm-up list is bundled yet (137 pipelines built on
  demand over the opening route, about 2 s in all).
- The shaders were compiled with this Mac's Metal 32023.883, not the one `dmsh_chain.lock` names.
- 60 fps, as the game runs on the Switch; no frame-rate control.

Open:

- Not checked: a play-through past the opening, combat, flight, the dungeons.
- Packaging gaps, as in `games/la` and `games/aoc`: the shader libraries stay in `Resources/game/shader-cache`
  instead of a signed `Contents/Helpers/ShaderData-*.bundle`, no pipeline archive is built at packaging, and
  repackaging refuses an existing `.app`.
- The iOS subtests of the runtime's tests need the iPhone SDK, which the Mac that checked this target lacks.
