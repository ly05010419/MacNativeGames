# Xenoblade Chronicles 3: changelog

## Status

Stages 1–4 are done (skeleton, code and data, rewrite, boot to `nnMain`). Version 1.0.0 is playable on the Mac: menus, the prologue movies, the opening, battles, free roam, sound, save and load (played by hand, 29 October 2026). Graphics have a few known gaps ([README.md](README.md#known-gaps)); the app and the update are next.

## Where the folder comes from

A copy of `games/ot0` at upstream commit `5ab07b7` (2026-10-28); see [third_party/COPIES.md](third_party/COPIES.md).

## Log

### 2026-10-29: stage 8, 2.2.1

Version 2.2.1 is built from the base NSP plus the owner's update NSP, side by side with 1.0.0 in one work folder (`VERSION=v221`).
- **Identification by content:** `extract_code.py` finds 2.2.1 by its ExeFS hashes and `merge_romfs.py` by its update Program NCA, both pinned in `assets/expected-v2.2.1.json`. The display version was read from the update's `control.nacp`.
- **Code:** only `main` changed. `sdk` and `subsdk0` are identical to 1.0.0, and their rewritten images came out byte-identical too.
- **One host change:** 2.2.1 calls `nn::oe::GetDisplayVersion` while starting. The SDK reads it through an applet proxy the host does not serve, so the game crashed at address 0. The host now answers with the build's version.
- **DLC menu:** "Purchase" calls `nn::ec::ShowShopAddOnContentList`, which opens the eShop applet. The SDK crashed there, since the host runs no applets. The shop now opens and closes at once, and the game reports no add-on content as before (seen in play, PR #1309).
- **Saves:** 2.2.1 opens a 1.0.0 save. Its save is 20 MiB instead of 10 MiB, and a save written by 2.2.1 may not load in 1.0.0.

Verified (Apple M5, macOS 27.0.1, 2.2.1, a copy of the 1.0.0 save):

| Check | Result |
|---|---|
| ExeFS, update Program NCA, merged RomFS (all six IVFC levels) | pass, pinned |
| `structck.py`, `equiv.py --min-eff 8` | OK for all three modules; 0 divergent sites; 300 of 300 mutations caught |
| Boot with A presses | title screen "Ver. 2.2.1", Continue loads the 1.0.0 save, a battle in Everblight Plain renders; 230 s with no crash |
| Played by hand | works (owner); "Purchase" in the DLC menu crashed the game, and after the fix it returns to the menu |

### 2026-10-29: stage 7, the app

`make app` builds `Xenoblade Chronicles 3.app` from the work folder, with the launcher copied from ot0. Changes for this game:
- The frame rate choice is gone. Ot0 needed it for its Unreal tick-rate patch, but this game holds itself to 30 fps and has no unlock. The app and `make boot` now both run with `TOTK_UNCAPPED=0`, so frames follow the game's own present interval on any display.
- TOTK's shortcut keys 2–4 are off. The owner's first session in the app showed cutscenes enlarged and shifted to the top-left corner. The log showed key 2 had turned on MetalFX frame interpolation, and interpolated frames are shown without the game's crop: the game draws its cutscenes at 1280×720 in the corner of a 1920×1080 picture. Keys 3 and 4 (2K modes, ultrawide) had nothing behind them for this game. The in-game help now lists key 1 only.
- The system fonts were found through absolute paths into the work folder. They are now relative to the folder the runtime runs from (the work folder, or the app's `game` folder), and `make app` bundles them. The game has not asked for them so far.

Verified (Apple M5, macOS 27.0.1, 1.0.0):

| Check | Result |
|---|---|
| `make app` with the game's icon | builds in about 30 s; `codesign --verify --deep --strict` passes, also after three runs |
| Work folder renamed, game started from inside the app with the launcher's settings (MetalFX on, 1920×1080 window) | boots, difficulty selection renders, 150 s with no crash |
| First run with an empty shader cache | one loading screen waits about 50 s for 92 pipelines and 10 libraries to compile |
| Second run | no wait; 30 fps |
| Started from Finder | works (owner): settings window, then the game through the opening |
| Cutscene after pressing 2 | enlarged into the top-left corner until restart; fixed by turning keys 2–4 off |

### 2026-10-29: stage 6, played by hand; save and load

The owner played the game through `make boot` (two fixes to the target: it opened no window and submitted no GPU work). They walked and fought in Everblight Plain, saved, quit, started again and loaded the save. All worked. Rare, brief screen tearing or thin black lines remain.

Verified (Apple M5, macOS 27.0.1, 1.0.0, Everblight Plain):

| Check | Result |
|---|---|
| Walking, camera and battle with keyboard or controller | works (owner) |
| Save, quit, start again, load | the save loads at the same place (owner) |
| Frame rate | 30 fps (frame time p50 33.2 ms, p95 33.7 ms); GPU about 6–7 ms per frame |
| Picture | rare tearing or thin black lines (not investigated yet) |

### 2026-10-29: stage 6, free roam

With A pressed every 10 s the game goes through the prologue, the first battle and into free roam in Everblight Plain, and runs for 15 minutes without a crash.

Verified:

| Check | Result |
|---|---|
| 900 s with A presses | free roam with party, minimap and objective; ends at the time limit |

### 2026-10-29: stage 6, characters in battle

The game computes bone matrices in vertex shaders drawn under rasterizer discard. The layer dropped those draws, so no characters were drawn. They now run with rasterization off, followed by a barrier inside the render pass so the mesh draws read finished matrices.

Verified:

| Check | Result |
|---|---|
| Frames 15500 / 16300 (first battle) | party, enemies and weapons render |
| Sound | working (the owner, by ear) |
| 575 s with A presses | ends at the time limit |

### 2026-10-28: stage 6, the first battle renders

The game uses a second texture pool and sampler pool next to its main ones and switches between them every frame. The NVN layer kept one table of texture ids, so the second pool's textures replaced the main pool's: the battle scene was black. Each pool now has its own id range.

Verified:

| Check | Result |
|---|---|
| Frame 15500 (first battle) | 3D scene, characters and HUD render |
| Frame 3400 (first movie) | still plays |
| 575 s with A presses | ends at the time limit |

### 2026-10-28: stage 6, first battle reached

A fragment shader with `textureGatherOffsets` offsets computed at run time stopped the game: the translator built a constant from them and SPIRV-Cross rejected it. The translator now reports that Metal has no non-constant texture offsets, so Ryujinx emulates those (constant offsets are unchanged).

Verified:

| Check | Result |
|---|---|
| Failing shader `1116625c…` | reproduced with SPIRV-Cross; after the fix it translates and compiles with Metal |
| 575 s with A presses | opening cutscene, then the first battle with HUD and tutorial windows; ends at the time limit |
| Frames 14500 / 16000 | battle HUD correct, 3D scene black (open) |

### 2026-10-28: stage 6, into the opening cutscene

After a movie's end of stream the host VP9 decoder kept handing input buffers back, so the game read its WebM extractor past the end about 250,000 times. The extractor then crashed when the game flushed (seeked) it after the second movie. The decoder now stops giving out input after end of stream until a flush, as MediaCodec does.

Verified:

| Check | Result |
|---|---|
| 330 s with A presses | both movies, then the in-engine opening cutscene with credits; ends at the time limit |
| Frame 9300 window screenshot | in-engine cutscene renders correctly |

### 2026-10-28: stage 6, both prologue movies play

- The movie picture was black because the composite pass sampled texture ids above 2048 and the layer kept the size of the last texture pool initialised (2048) instead of the largest. Both prologue movies now play.
- The third geometry program translates: its vertex part gets its own c1 data (BRX jump tables), reflects storage buffers against the vertex driver table, and binds vertex-stage state.

Verified:

| Check | Result |
|---|---|
| Frame 3400 window screenshot | first movie shown (was black) |
| Frame 8000 window screenshot | second movie shown |
| Geometry program `c01e3ade…` | `gsc` reproduced failing offline, translates after the fix, runs in the game |
| 300 s with A presses | reaches the end of the second movie (~290 s), then crashes in the game's WebM demuxer (open) |

### 2026-10-28: stage 6, into the first event

With A pressed every 10 s, the game goes from the difficulty selection into its first event and starts the event's VP9 movie (decoded on the host). Three gaps were fixed on the way:
- mid-pass scissored clears (their probe passed on this Mac and the path is switched on),
- blending left on for integer render targets (now off, as the GPU ignores it),
- depth `textureGatherOffsets` in the translator service.

The event's subtitles render; the picture under them is black.

Verified:

| Check | Result |
|---|---|
| `probe-gpu-midclear` on Apple M5 | pass, 9,216 checks |
| 230 s with A presses | difficulty selection, event `ev01005100`, movie `mv01005100.webm` opened, subtitles shown; stops at a geometry program the translator rejects |

### 2026-10-28: GPU BC7 encoding

A fragment shader writes BC7 blocks into a 16×64 BC7 sRGB texture through an image. Metal has no integer view of a compressed texture.
- **Shadow.** The image is bound to a block-sized RGBA32Uint shadow instead.
- **Copy.** After the draw, the blocks are copied into the BC texture through a buffer.
- **Ownership.** The texture is marked GPU-owned, so it is not re-uploaded from guest memory.

Verified:

| Check | Result |
|---|---|
| 120 s with button presses | the shadow is made once (4×16 blocks); no dropped-write message, no error |
| The encoded texture's content | not checked |

### 2026-10-28: geometry shaders

Metal has no geometry stage. A draw whose program has one now runs as:
- the vertex shader as a compute kernel,
- the geometry shader as a compute kernel,
- an indexed strip draw of the result.

The vendored Ryujinx translator makes the three shaders (new request stages `vsc` / `gsc` / `vpt`, patch `0009`). The NVN layer feeds them: vertex and index buffer textures, `vb_info`, output buffers sized from the reflection and the geometry header.

Point input only, which is what the first scene uses (two programs, 2304 points each, one quad per point).

Verified:

| Check | Result |
|---|---|
| Both geometry programs, every frame for 120 s, no `DM_NVN_GS_SKIP` | no crash, no pipeline or validation error; the game reaches the difficulty selection |
| Quads in the right place | not checked yet |

### 2026-10-28: transform feedback

Metal has no transform feedback, and Xenoblade Chronicles 3 uses it from its first scene.
- **Where the layout lives.** Each shader's control section holds the varying layout, and the game's own NVN driver turns it into Maxwell state. Both are described in [docs/reference/nvn-transform-feedback.md](docs/reference/nvn-transform-feedback.md).
- **Emulation.** The NVN layer computes the layout and the vendored Ryujinx translator emulates the capture: the vertex shader stores to storage buffers (patch `0008` keeps them in the bound descriptor set). The layer binds those buffers, gives each capturing draw its offsets, runs it without rasterization under rasterizer discard, and replays DrawTransformFeedback from the bytes written.
- **Translator.** `prebuilt/macos-arm64/totk_xlate` is rebuilt with these changes.

Verified:

| Check | Result |
|---|---|
| First scene, 120 s with `DM_NVN_GS_SKIP=1` | no crash; one captured draw per frame, 16 bytes; the capture buffer holds the shader's values (`0.1436, 0.8706, 16384, 0`) |
| Shaders without transform feedback | translated as before (the job has no `tfb`) |

### 2026-10-28: stage 5, the first picture

- **Language.** 1.0.0 supports British English but not American English (control.nacp). The runtime answered `en-US`, so the engine looked for `bdat/us/…`, a resource failed, and the game crashed a few frames in. The language is now chosen as Horizon does, among the languages the version supports (from the manifest).
- **Save size.** It now also comes from control.nacp.
- **Services.** `nn::prepo::AddSessionId` was added, and `ClearColori`/`ClearColorui` became the ClearColor record.
- **Result.** The game shows its loading screen, loads the first map and event, and waits on a letterboxed scene at a steady 30 fps until a button is pressed. It then binds a transform feedback buffer. Metal has neither transform feedback nor geometry shaders, and the game uses both. The vendored Ryujinx translator can emulate both, so they are the next step.

Verified:

| Check | Result |
|---|---|
| Boot without a language override | 3 minutes without a crash; loading screen and first scene in window screenshots |
| Integer clear | the game passes it |
| Transform feedback | stops with a clear message; `DM_NVN_TF_STUB=1` goes on to the first geometry shader (`unsupported NVN stage 2`) |

### 2026-10-28: stage 4, the game reaches `nnMain`

The runtime copied from ot0 built for this game with no address left to fill in. The first boot ran SDK startup and every constructor (main 805, subsdk0 45, sdk 112) and entered `nnMain`. Two calls ot0 never made were then added:
- `nn::fs::IsSaveDataExisting(Uid)`, in both overloads. Without it the SDK opened `fsp-srv`.
- `nn::hid::GetTouchScreenStates<16>`. Without it the game read hid shared memory the host does not map.

The game then:
- starts 39 threads,
- fills its memory pool (about 3.16 GiB) in 1 MiB blocks,
- brings up Metal, VI and a 1920×1080 swap chain,
- starts translating its shaders,
- stops at the first NVN feature the graphics layer lacks: sampling the stencil plane of a depth-stencil texture.

Verified:

| Check | Result |
|---|---|
| `make prepare` | 3 modules, 128,914 relocations, RomFS index of 246 files, the six shared fonts |
| `make runtime` (`-Werror`) | builds |
| Boot | `ENTER nnMain`; window, Metal device and shader translation start; stops at `stencil-plane texture sampling is not implemented` |

### 2026-10-28: stage 3, rewrite

`third_party/d2r/patch_nso.py` (`DM_RULESET=native DM_PRIVATE_GATES=0`) rewrites all three modules without any change to the rewriter:
- `main` is small (31.6 MiB mapped), so every patch area fits after its module.
- Only `sdk` has `svc`, counter and TLS-register sites; `main` (4,944 sites) and `subsdk0` (10,525) only use x18.
- `sdk` has 11,502 sites and 109 host gates.

`equiv.py` at its default of 2 effective trials per site missed one mutation control in `main`: a `nop` in place of `csel x18, x18, x0, lo` only shows when the carry is set. At 8 effective trials per site every module passes, so the documented command uses `--min-eff 8`.

Verified:

| Check | Result |
|---|---|
| `structck.py` | OK for sdk, subsdk0 and main, 0 failures; the independent llvm-objdump count matches the sites |
| `equiv.py --min-eff 8` | 0 divergent sites. main 4,944 sites / about 43,000 trials, with seeds 1, 2 and 3. sdk 11,393 / 97,704 and subsdk0 10,525 / 89,667, seed 1. 300 of 300 mutations caught in each run. |
| Determinism | a second rewrite is byte-identical (`*.patched.bin` and every `*.tsv`); SHA-256 pinned in `assets/expected-v1.0.0.json` |

### 2026-10-28: stage 2, code and data

1.0.0 ships its code in the base NSP. The base Program NCA (`edc8bdc5…`, 14.8 GB, key generation 13) holds both a complete ExeFS and a plain AES-CTR RomFS.
- **`extract_code.py`:** reads the update Program NCA when the NSP has one and the base game's otherwise.
- **`merge_romfs.py`:** the update is optional. With the base alone, it verifies and writes the base section.
- **Versions:** `versions.py` identifies 1.0.0 by its ExeFS hashes and its base Program NCA.
- **Modules:** `main`, `subsdk0` and `sdk` (SDK 12.3.9; `subsdk0` is the SDK multimedia library), the same set as Octopath Traveler 0. The manifest now lists them, and `prepare_runtime.py` reads the list from it.
- **Address space:** `main.npdm` asks for a 39-bit address space.
- **RomFS:** 246 files in 13.7 GiB:
  - 238 WebM movies,
  - 5 Wwise sound packages,
  - the engine's `bf3.ard`/`bf3.arh` archive.

Verified:

| Check | Result |
|---|---|
| FS header, ExeFS hash table and every ExeFS block | pass; 5 files pinned in `assets/expected-v1.0.0.json` |
| RomFS, all six IVFC levels | pass (900,774 level-6 blocks); size, SHA-256 and file count pinned |
| One changed byte in `sdk` | rejected: "its code matches none of 1.0.0" |
| A second extraction into the same work folder | refused, nothing overwritten |
| `games/xc3/tests` (manifest shape, identification by content, a changed or extra ExeFS file stops) | 4 tests pass |

### 2026-10-28: stage 1, skeleton

- Copied `games/ot0` whole except its README and CHANGELOG.
- Renamed every name and identifier for this game (`XC3_*` variables, `XC3_build`, `xc3-native-<version>`, `MacNativeGames/XC3`, `org.macnativegames.xc3`, title `010074F013262000`).
- Removed Octopath Traveler 0's Unreal frame-rate patch, its default, and its 1.0.3 and 1.0.4 manifests.
- `runtime/game_version.h` names one version, 1.0.0, and holds no addresses, so an address left over from another game cannot compile.

Verified:

| Check | Result |
|---|---|
| No reference to another game's folder (`git grep` for `../totk`, `games/ot0` and the rest) | comments and `COPIES.md` only |
| No Octopath Traveler 0 names left outside `COPIES.md` and the docs | none |
| Nothing changed outside `games/xc3` | `git diff --stat upstream/main -- . ':!games/xc3'` empty |
| `make shader-tools` (prebuilt `totk_xlate` checked by SHA-256, SPIRV-Cross at its pinned commit) | builds |
| `make gfx` (the NVN-on-Metal layer, 21 objects) | compiles, exit 0 |
| Scripts parse (`ast.parse` over `scripts/*.py`) | OK |
| `make runtime` | not run yet: it needs the generated tables from stage 2 |
