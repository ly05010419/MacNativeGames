# Luigi's Mansion 3 (work in progress)

Luigi's Mansion 3 1.2.0 and 1.4.0 running natively on Apple silicon, the way `games/totk` runs Tears of the Kingdom. It started
from that code (rewriter, loader, Horizon host, NVN on Metal) and, like `games/by3`, keeps its own copies of all of it
in this folder (`runtime/`, `third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`, `tools/`, `scripts/`), so changes
here never affect another game and nothing here reads `games/totk`.

**1.2.0 is playable**: title screen, menus, the intro movie and the game run with picture, sound, controller and saves,
stable over long sessions (tested on an M3 Max with macOS 26.4). 1.4.0 uses separate original-module and merged-resource
hashes; its validation record and the remaining gaps are below.

For 1.4.0, the native display adapter now defaults to a 2560×1440 scene and output, fixed scene scale, and a real
60 FPS target. Game Controls (Command+,) offers 30/60 FPS; resolution changes take effect on the next launch.
The menu reports measured G-buffer coverage and swapchain dimensions separately. Settings belong to LM3,
independently of Tears of the Kingdom. Frame generation and the inherited TOTK upscaling presets are disabled.

Command-line validation can select `LM3_RENDER_SIZE=1280x720`, `1600x900`, `1920x1080` or `2560x1440`, and
`LM3_TARGET_FPS=30` or `60`. `LM3_RENDER_SIZE=0` preserves the game's original dimensions and dynamic resolution.
The native display patches require the pinned 1.4.0 build; they are not applied to 1.2.0. A target rate is a limit,
not a promise that every scene meets that rate.

You must own the game: use your own game files and the `prod.keys` dumped from your own Switch. Nothing from the game
is in this repository; the generated folders (`extracted/`, `rewritten/`, `build/`, `work/`) are ignored by git.

## Inputs

- Base game `0100DCA0064A6000` as a card image (XCI: the shared fonts come from its system update partition) and
  update 1.2.0 or 1.4.0 `0100DCA0064A6800` (NSP).
- A same-game combined XCI is supported with `COMBINED=1`: the base and update are selected by their section type
  and verified content metadata, independently of entry order. DLC entries are ignored, not mounted.
- Cards with no shared fonts need `SHARED_FONTS=<folder>` containing the player's six previously extracted `.bfttf`
  files and their `manifest.json`. Their sizes, headers and SHA-256 are verified before local copies are made.
- The code comes from the update; the RomFS is the base merged with the update (pinned hactool, BKTR).

## Build and play

Validated on Apple silicon with macOS 27.2. Building needs the Xcode command line tools and Python 3.14;
`tools/deps` builds the pinned codecs without Homebrew. From
`games/lm3`:

```sh
python3.14 -m venv .venv && .venv/bin/pip install -r requirements.txt
make extract VERSION=v140 ROM="<base>.xci" UPDATE="<update 1.4.0>.nsp" KEYS=prod.keys
make rewrite VERSION=v140                                                # the four code modules for Apple silicon
make app VERSION=v140 ROM="<base>.xci" KEYS=prod.keys                     # checkout launcher
open "Luigi's Mansion 3.app"
```

`ROM`, `UPDATE` and `KEYS` can go in a `local.mk` instead (git-ignored). `make verify-rewrites VERSION=v140` re-runs the rewriter's
equivalence check (emulator, random states) against the selected original inputs. Use `VERSION=v120` for 1.2.0.

For a **fresh 1.4.0 combined-XCI build**, put these paths in `local.mk` (paths on the right need no quotes):

```make
VERSION = v140
COMBINED = 1
ROM = /path/to/combined-1.4.0.xci
KEYS = /path/to/prod.keys
SHARED_FONTS = /path/to/previously-extracted/shared-fonts
APP = /Applications/Luigi's Mansion 3.app
```

Then run `make extract`, `make rewrite`, `make app`. `UPDATE` defaults to the same container in combined mode.
Use `VERSION=v120` for the existing 1.2.0 targets.
Existing extraction outputs are never overwritten; after changing only runtime/graphics sources, resume with
`make app VERSION=v140`. A selected-version header forces the correct
runtime tables and resource configuration when switching versions. The input pins live in `scripts/game_versions.py`;
another version must not be passed off as one of these versions by changing its directory name.

Some rights-free combined containers have a different encrypted NCA header from the original NSP and therefore a
different whole-NCA hash from the CNMT. The merge report records both hashes and whether they match. ExeFS/PFS0,
NSO segments and all six merged IVFC levels must still verify; the 1.4.0 runtime also checks its pinned original modules
and merged RomFS. These checks do not authenticate Nintendo's RSA signatures.

The app is a small launcher around this checkout. Saves and the shader cache live in
`~/Library/Application Support/MacNativeGames/LM3`; logs live in `~/Library/Logs/MacNativeGames/lm3.log`.
Standalone packaging is a separate contribution.

## What differs from Tears of the Kingdom

- **SDK 7.3.2**: an older `nn::init` start sequence, rtld global symbol binding, and its own services (audout, the
  movie library) in `runtime/`.
- **LM3 display patches**: 1.4.0 uses its own NLG initialization, resource-resize and frame-limiter paths. They are
  guarded by the full original main build ID and original instruction/data words. The limiter changes from 33.333 ms
  to 16.167 ms at a 60 FPS launch; the engine continues using elapsed time. The host's cap and present-interval query
  agree, including on higher-refresh screens. Code/constants are patched before the ordinary RX/R protections are sealed.
  The renderer uses a fixed scene scale of 1.0 and one full-size descriptor per render target, and allows its window
  resources to allocate at the same startup size. Native 1440p adds a single 512 MiB reservation, consistently accounted
  for by the game's private texture pool, main heap and reported application pool; the 4 GiB guest heap cap remains.
  The available-memory check also requires the enlarged heap. Smaller modes retain their original budgets.
  It does not use TOTK's AGL HeapDef overlays or blanket NVN texture scaling. Limiter behavior was cross-checked against
  [nxnative's LM3 notes](https://github.com/seedds/nxnative/blob/main/dm/games/0100DCA0064A6000/README.md#frame-rate)
  and verified against this original main module.
- **Movies**: VP9 decoded on the host with libvpx into the game's NVN texture (`runtime/movie_host.c`).
- **Virtual cores**: three cores with preemption, ideal-core displacement and an anti-starvation rule, so the game's
  worker threads cannot starve its main thread.
- **Resized texture caches**: resize notifications travel in replay order. Superseded storage layouts with no live
  aliases expire after 32 unused submissions instead of the ordinary 600-submission cache; actual release still waits
  for GPU completion. Recent multipass/history reads and returning sizes retain their content. A pool being finalized
  clears its resize history. The 8 GB GPU budget remains enabled.
- **VI history**: this native layer retains no VI frame-history records. Its SDK adapter returns an empty list (and the
  SDK's eight-entry capacity for a capacity query), rather than entering the guest implementation with an opaque host layer.
- **NVN on Metal**: textures in CPU-no-access pools are mirrored, one-stage programs are merged per pair, depth/colour
  aliasing gets a colour twin, integer render targets never blend, and the sRGB swap chain is presented as stored.
- **Shaders**: the translator rejects non-constant texture offsets (`QueryHostSupportsNonConstantTextureOffset`) and
  pre-translates every program the game registers.

## Known gaps

- This branch needs the separate SCREEN-blend fix for the vacuum-upgrade CG effect; without that fix, the effect can stop the game.
- Triangle fans (primitive type 6) are not drawn yet, and primitive restart uses Metal's all-ones index only.
- One texture-to-texture copy shape (1920x1080 to 160x90) is not implemented.
- The original rendering path (`LM3_RENDER_SIZE=0`) sweeps between 1920x1080 and 1600x900. The 1.4.0 native-resolution
  path fixes scene scale at 1.0; it costs more GPU time at larger sizes.
- Only versions 1.2.0 and 1.4.0 have pinned inputs. 1.2.0 was not rebuilt during the 1.4.0 work because its input was unavailable.
- The existing `probe-gpu-compressed-alias` fails its non-compressible depth/colour-alias subcase on this Mac. The same
  failure was reproduced with the repository's original graphics source; it is not resolved by the resized-cache fix.

## Local verification

```sh
.venv/bin/python -m unittest discover -s tests -v
make verify-rewrites VERSION=v140
# LLVM_BIN must contain llvm-objcopy and llvm-objdump.
for m in main subsdk0 subsdk1 sdk; do
  .venv/bin/python third_party/d2r/structck.py rewritten/v140 "$m" extracted/v140/exefs/"$m"
done
MTL_DEBUG_LAYER=1 build/lm3-native rewritten/v140 probe-gpu-storage-view
MTL_DEBUG_LAYER=1 build/lm3-native rewritten/v140 probe-gpu-texture-retire
build/lm3-native rewritten/v140 probe
build/lm3-native rewritten/v140 probe-save
```

The storage probe's `TOTK_PROBE_NEGATIVE=8` control skips resize notifications and must fail with
`retained superseded layout`. The stack-switch guard checks all ten trampoline instructions, the original instruction,
key page/slot and return PC; its synthetic tests flip every instruction bit and reject altered identity values.
The actual code rewriter and rewritten game modules are unchanged by this guard correction.

### 1.4.0 opening check before the native display adapter (2026-11-11)

On macOS 27.2, Apple M1 Max with 32 GiB, the combined 1.4.0 XCI reached the opening movie, hotel lobby,
night-time rooms and corridors. A first session ran for over 12 minutes, closed normally, and its save loaded in
a second session of over nine minutes; that session reached playable vacuum use. Controller input, movement and
vacuum effects were observed at about 30 FPS, with shader-compilation stalls on first encounters. Sampled Metal
allocation peaked at 3,206 MB in the first session and 3,098 MB in the second, below the unchanged 8,000 MB guard.
Neither session aborted. This is opening-game coverage, not a full-game compatibility claim.

The 12 input/checker unit tests, all four rewrite equivalence and structural checks, storage-view and texture-retirement
GPU probes, native cross-module call probe and isolated save probe passed. The storage probe's negative control failed
as expected. The repository suite ran 1,663 tests with 172 skips; three settings cases initially timed out while another
game was running. After that game closed, the affected settings test passed for all five game-version configurations.
The pre-existing compressed-alias probe failure listed above remains. Logs and the local verification report are in the
ignored `analysis/validation-2026-11-11/` directory.

### Native 1440p / 60 FPS check (2026-11-11)

The same Mac subsequently loaded the corridor save and reached the hotel lobby with the native display adapter.
Both integer G-buffer colours, matching depth and actual viewport cover 2560×1440; an independent pass dump and a
native 2560×1440 GPU screenshot confirm it. All three swapchain images were sampled repeatedly at 2560×1440,
with valid, non-overlapping ranges in the enlarged private pool and no crop errors. The guest heap was independently
observed at 0xdbe00000. The GPU budget remains 8,000 MB.

A continuous 600-frame gameplay sample had game-frame p50/p95 16.682/16.809 ms, GPU p50/p95 9.527/10.451 ms and
displayed-interval p50/p95 16.680/16.681 ms. That sample also includes a first-time library/pipeline creation and a
224 ms game-frame maximum; this is a 60 FPS target with occasional stalls, not a whole-game locked-60 claim.
Sampled GPU allocation reached 3,038 MiB in the completed player session without a budget abort. These figures describe
this bounded gameplay route.

The 23 local unit tests, memory probe, NVN/pacing probe, storage-view and texture-retirement probes passed. An
undersized application-pool override and the intentionally wrong present-interval query were both rejected by their
negative controls. The original compressed-alias limitation remains. Logs, timings, buffer ranges and the native capture
are kept in the ignored `analysis/quality-2026-11-11/` directory. Separate 30 and 60 FPS runs loaded the same corridor
save, with physical controller discovery disabled only for the explicitly marked test process. A two-second left walk
and a 1.5-second aim input produced similar endpoints in both modes, with no doubled movement speed. Both runs loaded
the save and closed normally. These are historical integration results; this contribution branch does not include
the separate SCREEN-blend or two-controller changes. iOS was not built or played during that validation.
