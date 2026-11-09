# The Legend of Zelda: Link's Awakening (Mac, experimental)

A native Mac build of Link's Awakening 1.1.0 (`01006BB00C6F0000`, v131072) from the player's own
files, on a copy of the Age of Calamity target and nxnative's profile of the game
([provenance](third_party/COPIES.md)). Experimental: played by hand around Mabe Village, not yet played
through on the Mac.

## Inputs

The base game (cartridge dump or NSP) and update 1.1.0, with `prod.keys`. `scripts/extract_la.py`
chooses the programs by their content metas, checks header signatures, each program's SHA-256 and
every RomFS file through the update's hash tree, and writes `ExeFS/` and `RomFS/` to a new folder.
The build then checks that folder against `assets/expected-1.1.0.json`: the identity nxnative's
profile gives (`external`) and the files of the first checked extraction (`observed`), which say only
that a later build reads the same files.

One cartridge dump's base content meta fails its signature; `assets/integrity-exceptions.json` names
it by SHA-256. With it, the base game's version 0 is declared by an unsigned meta: it is not
authenticated, and the program is trusted through its own signature and the update's hash tree.

## Build

Apple silicon, macOS 26 or later, Xcode with its Metal toolchain, .NET SDK 10, Python 3 with `lz4`,
`capstone`, `fonttools` (and `backports.zstd` before 3.14), `spirv-cross`, `zstd` and `make`.

```sh
python3 scripts/extract_la.py --base "<base>.xci" --update "<update 1.1.0>.nsp" --keys prod.keys \
  --libhac libhac.0.19.0.nupkg --work-dir "<work>" --output "<extracted>" --evidence "<evidence>"
python3 scripts/build_la.py "<extracted>" --work-dir "<work>" --output "<new>.app"
```

Work, output and evidence stay outside the checkout. nxnative records two pixel shaders of
`postprocess.bfsha` that did not translate there; `assets/shader-exceptions.json` lets exactly those two be
absent from a package, checked by their origin. It is an allowance for that case: on the Mac it was built
on, both translated, so that build did not use it.
Saves and logs go to `~/Library/Application Support/The Legend of Zelda Link's Awakening/`.

## Sharing shaders

While you play, the app keeps the shaders and pipelines it had to build on the spot in `pso_user.tsv`
beside the save folder, and builds them ahead at the next start. Settings → Shader list → **Export shader
list…** saves the ones the app's own list lacks to a file you choose (keys only, no game data). Nothing is
sent: send that file to the maintainer by hand. Exporting changes nothing in your own list.

The maintainer merges contributions into the bundled list, the current list first:

```sh
python3 scripts/pso_seed.py --output new_pso_manifest.tsv --shader-map "<work>/source/dm/games/01006BB00C6F0000/shader_map.tsv" \
  runtime/games/01006BB00C6F0000/pso_manifest.tsv la-shaders-*.tsv
```

Every row is checked, the list is never cut to fit its 1,500-line limit, and `--shader-map` refuses keys that
are not shaders of this game's 1.1.0 build. That shows the keys exist in 1.1.0, not that a file was recorded
on it: versions can share keys.

## State and work to do

Checked on an M5 Pro Mac (macOS 26.5.2). On 2026-11-07, on a disposable profile with scripted input: the
opening movie and intro, the title (L+R), a new file, Marin and Tarin's house and their conversations,
Mabe Village, and a save that loads again after quitting. On 2026-11-08, played by hand with a game
controller and sound; what was measured is in [docs/validation.md](docs/validation.md).

Fixed:

- The opening movie and the scene on the beach where Marin finds Link were out of focus and flickered: a
  movie picture now stays in its slot until the game releases it. Seen right by eye.
- Age of Calamity's frame-rate control is gone: Link's Awakening paces itself at 60 FPS, and a target saved
  by an earlier build is ignored (checked in a run).
- Outside Marin and Tarin's house the game ran at 60 FPS with every display tick presented (30 s of
  walking). An earlier note of about 40 FPS in Mabe Village was not seen again; its cause is not known.
- Talking to Marin again: no lock-up of the dialogue was reproduced with separate presses (the box closes); a
  burst of presses is compatible with the box closing and opening again (those frames were not captured).
  Scripted presses used to merge into one when sent back to back; they now wait until the game sees each release.
- A game controller and the sound were checked by hand.

Known limits:

- Stutter the first time a place needs a pipeline: the app ships a warm-up seed recorded on one route
  (`runtime/games/01006BB00C6F0000/pso_manifest.tsv`); places not on it still build pipelines when first
  met, and players can export what their Mac built ([Sharing shaders](#sharing-shaders)). No pipeline archive.
- The shaders were compiled with this Mac's spirv-cross and Metal 32023.883, not the ones
  `dmsh_chain.lock` names.

Open:

- In one session the host's display link stalled eight times (0.3 to 1.2 s) and none in its twin; the cause
  is not known ([validation](docs/validation.md)).
- A changed byte inside a cartridge NCA's hashed region stops `nx_extract` with an unhandled exception
  (exit 134) instead of a named exit code.
- Not checked: a play-through past the village.
- Packaging gaps against the repository's standard (the same in `games/aoc`, which this port is copied from): the shader
  libraries stay in `Resources/game/shader-cache` instead of a signed `Contents/Helpers/ShaderData-*.bundle`; no
  pipeline archive is built at packaging, so warm-up happens at run time and the title cannot skip it; verified digests
  are not reused between steps; repackaging refuses an existing `.app` instead of keeping the folder and replacing its
  contents; there is no runtime-only fast update. Not done yet.
