# Luigi's Mansion 3 (work in progress)

Luigi's Mansion 3 1.2.0 running natively on Apple silicon, the way `games/totk` runs Tears of the Kingdom. It started
from that code (rewriter, loader, Horizon host, NVN on Metal) and, like `games/by3`, keeps its own copies of all of it
in this folder (`runtime/`, `third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`, `tools/`, `scripts/`), so changes
here never affect another game and nothing here reads `games/totk`.

**It is playable**: title screen, menus, the intro movie and the game run with picture, sound, controller and saves,
stable over long sessions (tested on an M3 Max with macOS 26.4). Known gaps are listed below.

You must own the game: use your own game files and the `prod.keys` dumped from your own Switch. Nothing from the game
is in this repository; the generated folders (`extracted/`, `rewritten/`, `build/`, `work/`) are ignored by git.

## Inputs

- Base game `0100DCA0064A6000` as a card image (XCI: the shared fonts come from its system update partition) and
  update 1.2.0 `0100DCA0064A6800` (NSP).
- The code comes from the update; the RomFS is the base merged with the update (pinned hactool, BKTR).

## Build and play

Needs macOS 26 on Apple silicon, the Xcode command line tools and Python 3.14 ([python.org](https://www.python.org/downloads/macos/)); opus and libvpx come with the build (pinned sources, made once per Mac by `tools/deps`; no Homebrew). From
`games/lm3`:

```sh
python3.14 -m venv .venv && .venv/bin/pip install -r requirements.txt
make extract ROM="<base>.xci" UPDATE="<update 1.2.0>.nsp" KEYS=prod.keys   # code, RomFS, shared fonts (builds hactool)
make rewrite                                                              # the four code modules for Apple silicon
make app ROM="<base>.xci" KEYS=prod.keys                                  # Luigi's Mansion 3.app
open "Luigi's Mansion 3.app"
```

`ROM`, `UPDATE` and `KEYS` can go in a `local.mk` instead (git-ignored). `make verify-rewrites` re-runs the rewriter's
equivalence check (emulator, random states): 0 divergent sites in all four modules, 24,300 sites in `main`.

The app is a small bundle that runs the game from this folder; do not move or delete it. Saves and the shader cache go
to `~/Library/Application Support/MacNativeGames/LM3`, the log to `~/Library/Logs/MacNativeGames/lm3.log`. Its icon is
the game's own, from the game's control data.

**First launch:** the game ships about 5,000 shader programs. They are translated for Metal in the background from the
title screen on and kept in the shader cache, so the first launch is busier and an unseen shader can still cause a short
stutter the first time.

**Cheats:** put an Atmosphère cheat file for this build ID (`98B7F5E7E13B1A4D`) at `build/cheats.txt` before
`make app` and the app gets a Cheats menu; nothing is downloaded.

## Shared shader identifiers

`assets/shader_list.tsv` records LM3 shader keys, non-default binding states and portable pipeline descriptors only.
It contains no translated shader sources, compiled libraries, game files or personal data. The initial observations
come from LM3 1.4.0 on a Mac using this game's `totk-native-v4-unverified` translation chain; they do not establish
whole-game coverage or compatibility with another game's newer translation chain. The rendering changes for 1.4.0
are separate from this identifier-only list.

One recorded render pipeline uses advanced blend mode 22 (SCREEN) from the accompanying LM3 rendering fix.
Collecting or exporting that recipe does not make it executable by the base runtime without that fix.

The local `scripts/shader_list.py` keeps the old LM3 binding rules, including integer vertex attributes and fragment
outputs. It drops states served by default 2D/float bindings, canonicalizes names from the recorded dependencies,
rejects pipeline process pointers and checks the 544-byte pipeline ABI against this game's runtime in its tests.
It also checks reserved blend bits, stream/attachment bounds and specialization fields; it is not a complete
runtime semantic validator and does not establish that every format or blend mode is supported by a given GPU.
From `games/lm3`:

```sh
python3 scripts/shader_list.py merge assets/shader_list.tsv --cache "<LM3 translation cache>" --pso-list "<LM3 profile>/pso_user.tsv"
python3 scripts/shader_list.py stats assets/shader_list.tsv
python3 -m unittest discover -s tests -p test_shader_list.py
# Resolve recipes against translations already made from this player's selected game files.
python3 scripts/shader_list.py export assets/shader_list.tsv build/pso_manifest.tsv --cache "<LM3 translation cache>"
```

Use LM3's own cache, not another game's. `merge` reads `.variants` and reflection metadata, plus an explicitly selected
PSO list and the cache's `pso_collected.tsv`; it never reads MSL, SPIR-V, native shader code or saved game progress.
`export` skips recipes unavailable in the selected cache and reports their count. It does not translate shader code or
download anything. The build does not yet consume this list automatically, and LM3 is not yet registered with the
website shader inbox. This is a per-game collection and validation tool, not an automatic-upload integration.

## What differs from Tears of the Kingdom

- **SDK 7.3.2**: an older `nn::init` start sequence, rtld global symbol binding, and its own services (audout, the
  movie library) in `runtime/`.
- **No game patches**: the game runs unmodified, at its own 30 FPS and with its own dynamic resolution. Render targets
  the game re-initializes at a new size without finalizing the old ones are released by the graphics layer, so the GPU
  memory stays flat.
- **Movies**: VP9 decoded on the host with libvpx into the game's NVN texture (`runtime/movie_host.c`).
- **Virtual cores**: three cores with preemption, ideal-core displacement and an anti-starvation rule, so the game's
  worker threads cannot starve its main thread.
- **NVN on Metal**: textures in CPU-no-access pools are mirrored, one-stage programs are merged per pair, depth/colour
  aliasing gets a colour twin, integer render targets never blend, and the sRGB swap chain is presented as stored.
- **Shaders**: the translator rejects non-constant texture offsets (`QueryHostSupportsNonConstantTextureOffset`) and
  pre-translates every program the game registers.

## Known gaps

- Triangle fans (primitive type 6) are not drawn yet, and primitive restart uses Metal's all-ones index only.
- One texture-to-texture copy shape (1920x1080 to 160x90) is not implemented.
- The app has no settings window yet (window size, full screen, frame rate).
- The dynamic resolution never settles: it sweeps between 1920x1080 and 1600x900, so the picture's sharpness varies.
- Only version 1.2.0 is supported.
