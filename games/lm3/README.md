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
- **SCREEN blending**: advanced state retains the NVN object's eight-byte layout and survives recorded command
  snapshots. An enabled SCREEN draw with uncorrelated coverage and a normalized destination uses exact RGBA blend
  factors. A straight-alpha source gets a cached fragment variant that clamps UNORM/sRGB input and premultiplies
  only the selected RGB outputs; alpha and the other outputs remain intact. This composes with depth-export removal
  and sampler-border handling. Other active advanced modes and overlap/destination combinations still fail explicitly.
- **Shaders**: the translator rejects non-constant texture offsets (`QueryHostSupportsNonConstantTextureOffset`) and
  pre-translates every program the game registers.

## SCREEN blend validation

`probe-gpu-screen-blend` exercises the production recorded-state and pipeline paths using synthetic shaders and GPU
readback. It covers premultiplied/straight input, linear/sRGB targets, clamping, multiple outputs, early returns,
depth-output removal, integer/masked targets, state lifetime and pipeline reuse. Negative controls 45/46 detect wrong
factors or a bypassed straight-alpha variant; 47/48/49 reject unsupported active overlap, destination and modes.
`LM3_BLEND_DIAG=1` enables bounded first-use blend/source diagnostics; these are off by default, while unsupported
active-state failures always report their reason.
`probe-gfx` also checks the setters/getters, invalid enums (negative controls 91/92), opaque-state canaries and command
snapshots. For a runtime built from the player's verified 1.2.0 inputs:

```sh
TOTK_SLOT_OWNER=codex MTL_DEBUG_LAYER=1 build/lm3-native rewritten/v120 probe-gpu-screen-blend
TOTK_SLOT_OWNER=codex TOTK_PROBE_NEGATIVE=45 MTL_DEBUG_LAYER=1 build/lm3-native rewritten/v120 probe-gpu-screen-blend
```

On 11 November 2026, the same SCREEN implementation fixed the vacuum-upgrade animation in a separate local 1.4.0
build on an M1 Max with macOS 27.2. The player confirmed the animation completed and controls returned; the synthetic
GPU probe passed 216,548 checks. That is historical Mac coverage, not a fresh 1.2.0 playthrough or an iOS result.
Support for extracting/building 1.4.0 is a separate change; this repair does not add its input pins or display patches.

This SCREEN-only branch has not been built or played with 1.2.0 inputs; those inputs are not available in this checkout.
macOS syntax checks pass for the changed graphics sources and probe entry points. The shared Metal syntax check for
iOS stops on two existing unavailable pixel formats (`MTLPixelFormatDepth24Unorm_Stencil8` and
`MTLPixelFormatX24_Stencil8`); the unmodified base has the same errors. No complete iOS app build or device playtest
has been performed for this change.

## Known gaps

- Triangle fans (primitive type 6) are not drawn yet, and primitive restart uses Metal's all-ones index only.
- One texture-to-texture copy shape (1920x1080 to 160x90) is not implemented.
- The app has no settings window yet (window size, full screen, frame rate).
- The dynamic resolution never settles: it sweeps between 1920x1080 and 1600x900, so the picture's sharpness varies.
- Only version 1.2.0 is supported.
