# Xenoblade Chronicles 3: changelog

## Status

Stages 1–4 are done (skeleton, code and data, rewrite, boot to `nnMain`). Version 1.0.0 is playable on the Mac: menus, the prologue movies, the opening, battles, free roam, sound, save and load (played by hand, 29 October 2026). Graphics have a few known gaps ([README.md](README.md#known-gaps)); the app and the update are next. The work plan is [docs/PLAN.md](docs/PLAN.md).

## Where the folder comes from

A copy of `games/ot0` at upstream commit `5ab07b7` (2026-10-28); see [third_party/COPIES.md](third_party/COPIES.md).

## Log

### 2026-10-30: black, flickering plants fixed

Grass and bushes showed black patches that changed every frame (seen in play, 2.2.1, at every render scale). The game captures the plants' wind-bent shapes with transform feedback and draws them from that buffer in the same render pass, first into depth (a pre-pass), then in colour with a depth test EQUAL. The graphics layer put no barrier after a capturing draw, so the pre-pass read the previous frame's shapes. The colour pass then failed the EQUAL test wherever the wind had moved the plant.
- **`gfx/nvn/nvn_mtl.m` `draw()`:** the vertex→vertex barrier that already followed vertex-only draws now also follows transform feedback captures.
- **Pass dump:** new `Y` lines (vertex-buffer binds as replayed) and `T` lines (transform feedback binds and begin / end). `XC3_POSDUMP_MAX` sets how many draws the position dump logs. `DM_NVN_SKIP_FS` takes up to 512 shader keys, for bisecting a scene.

Verified (2.2.1, M5, the player's save at Yzana Plains and the main game's save in Everblight Plain):

| Check | Before | After |
|---|---|---|
| Plants, 6 shots, original scale | black patches in every shot | none |
| Plants, 6 shots, full scale | black patches | none |
| Everblight Plain | right | right, no GPU errors |
| Frames over 25 s | 750–752 | 750 |

### 2026-10-30: stage 13, full render resolution (2.2.1)

The game draws its scene at half the output size, 960×544 of 1920×1080, as on the Switch. The scale comes from two start values in the renderer (0.5 in one mode, 0.53 in the other) that the game sets with code immediates.
- **`runtime/game_patches.c`:** an optional built-in patch sets both to one scale, 1.0 or 1.328 (three sites in `game_version.h`, each checked against the loaded word; a mismatch stops the start). 2.2.1 only.
- **Panel:** "Render resolution (next start)": Original (960×540), Full (1920×1080) or 2K (2552×1440, scaled down by the game to its 1080p picture), saved in `render-scale.txt`; `XC3_RENDER_SCALE=original|full|2k` decides first. Off by default.
- **Not offered:** 2.0 (3840×2160). Metal stopped taking the game's work after GPU errors, and the picture froze.
- **`runtime/mem_scan.h`** (copied from `games/totk`): memory scan, write watch and code breakpoints through the diagnostic command file; it found the source.

Verified (2.2.1, M5, title screen and the main game's save in Everblight Plain):

| Check | Original | Full | 2K |
|---|---|---|---|
| Scene targets (pass dump, free roam) | 235 at 960×544 | 237 at 1920×1080 | 243 at 2552×1440 |
| Picture | right | right, sharper at 1:1 | right; one run showed a bright streak, also seen in play at the original scale |
| Frames over the same 25 s | 752 | 751–753 | 750 (30 fps held) |
| GPU errors | none | none | none |
| 1.0.0 with `render-scale.txt` set | starts, original scale | — | — |

### 2026-10-29: stage 16, startup shaders bundled with the app

A fresh app translated every shader while playing. The first entry into Future Redeemed waited on about 10,000 of them, and a loading screen in the main game waited 50 s once.
- **`scripts/prepare_shader_seed.py` (`make shader-seed`):** builds `build/shader-seed` from the translation caches this Mac already has (the work folder's and the app's). It takes the variant indexes, MSL and reflection of every variant met in play, with SHA-256 in `seed-manifest.json`, in the format the graphics layer already imports (`gfx/nvn/totk_shader_seed.h`).
- **`make app`:** bundles the seed. It holds game-derived data and stays in the work folder and the local app, never in the repository.
- **No pipeline list:** the runtime waits at start until a seed's pipeline list is built, and with every pipeline met in play that took 35 s. The shaders are still compiled in the background from the start.

Verified (2.2.1, empty shader cache, the main game's save):

| Check | Result |
|---|---|
| Seed | 12,867 shaders, 38,477 files, 360 MB, built in about 10 s |
| Import at first start | 7 s; start-up wait 0.4 s (35 s with the pipeline list) |
| Shaders translated while playing | 8 (geometry as compute), against thousands before |
| First 93 s | all shaders compile in the background on 8 threads; a save loading in that time took 15 s |

### 2026-10-29: the enlarged picture after cutscenes (MetalFX output)

With the app's MetalFX output stage on, the picture stayed enlarged into its top-left corner after a 1280×720 cutscene. It was seen in play after the main game's opening (first app session) and after Future Redeemed's opening.

The game's own frames were right. New diagnostic `TOTK_DRAWABLE_SHOT` saves what goes to the window, and it showed the output stage drew the top-left 1280×720 of the 1920×1080 frame. MetalFX's content region (`inputContentWidth` smaller than the colour texture) does not hold in that sequence. A scaler built for the content size but reading the larger texture drew the picture in the corner instead.

Fix in `gfx/nvn/totk_mfx_up.m`: when the game's crop is smaller than its texture, the crop is copied into a texture of exactly its size first. Each content size has its own cached scaler, and a scaler always reads a whole texture of its input size.

The earlier diagnosis (key 2's frame interpolation) was wrong: interpolation never made a frame in XC3. Keys 2–4 stay off anyway, since the panel has replaced them.

Verified (main game opening replayed with the app's settings, `TOTK_DRAWABLE_SHOT` before and after the fix):

| Frame | Before | After |
|---|---|---|
| Cutscene (crop 1280×720) | full window | full window |
| Battle after it (1920×1080) | top-left 1280×720 enlarged | full window |

### 2026-10-29: stage 17, add-on content

Version 2.2.1 sees and loads the player's own four add-ons (Expansion Pass volumes 1–4; volume 4 is Future Redeemed).
- **`scripts/extract_aoc.py`:** decrypts each add-on's data NCA with the NSP's common ticket and verifies all six IVFC levels. It writes the files to `extracted/aoc/<index>/romfs`, and anything not matching `assets/expected-aoc.json` stops.
- **The runtime:** `nn::aoc` lists the add-ons present, and `nn::fs::MountAddOnContent` mounts their folder read-only. This is for 2.2.1 only; `XC3_AOC=0` hides them.
- **The app:** `make app VERSION=v221` bundles them.

Verified (Apple M5, macOS 27.0.1, 2.2.1, a copy of the save):

| Check | Result |
|---|---|
| Four add-ons: data NCA, all six IVFC levels | pass (2, 6, 6 and 80 files; 40 MB, 201 MB, 181 MB, 4.0 GB) |
| The game's view | "4 present", `aoc1`–`aoc4` mounted; the title screen offers "To DLC: Future Redeemed" |
| Expansion Pass menu | volumes 1–4 "Now Available!"; volume 1's items with "Receive Contents" |
| Volumes 1–3 in game (in play) | work |
| Future Redeemed played | not yet: not reached in play; scripted presses with fixed timing do not reach volume 4 in the menu |

### 2026-10-29: stage 12, TotK's settings panel under key 1

Key 1 opens TotK's in-game settings panel (Display, Input, Key bindings). The panel and the input host it needs were copied from `games/totk` at upstream `c330ec7`, file by file (`third_party/COPIES.md`). This port's own runtime and graphics fixes stay.

What works in XC3:
- **Display:** the overlay (all / FPS + playtime / off) and the FPS graph.
- **Input:** the controller type, mouse mode and its sensitivity, rumble and its strength.
- **Key bindings:** the keyboard, mouse and controller layouts, with drawings and a list. Changes apply at once and are saved.

Some rows show one fixed value until their stage:
- frame lock: "30 fps (the game's own)", stage 14;
- frame interpolation, stage 11;
- resolution: "1080p", stage 13;
- aspect ratio: "16:9";
- edge smoothing and gyro aiming: not in this game.

TotK's shader contribution button is hidden. The default keys are the ones this port had (J K I U for A B X Y…), not TotK's, and actions are named after the Switch buttons. Settings are kept next to `controls.txt`: in `XC3_build/user-data` for `make boot`, and in the app's data folder for the app.

Verified (Apple M5, macOS 27.0.1, 2.2.1; `TOTK_PANEL_SHOT` pictures of each page):

| Check | Result |
|---|---|
| Key 1 (script) | the panel opens; all three pages draw |
| Scripted A presses with the new input host | title, Continue, the 1.0.0 save loads in Everblight Plain |
| Both versions build | yes |
| By hand, in play (app and `make boot`) | open/close (key 1, Esc, the close button, L3 + R3), overlay and FPS graph, controller type, rumble in game, rebinding, settings kept after a restart, default keys as before: all work. Rumble strength not tried. |

### 2026-10-29: stage 8, 2.2.1

Version 2.2.1 is built from the base NSP plus the player's update NSP, side by side with 1.0.0 in one work folder (`VERSION=v221`).
- **Identification by content:** `extract_code.py` finds 2.2.1 by its ExeFS hashes and `merge_romfs.py` by its update Program NCA, both pinned in `assets/expected-v2.2.1.json`. The display version was read from the update's `control.nacp`.
- **Code:** only `main` changed. `sdk` and `subsdk0` are identical to 1.0.0, and their rewritten images came out byte-identical too.
- **One host change:** 2.2.1 calls `nn::oe::GetDisplayVersion` while starting. The SDK reads it through an applet proxy the host does not serve, so the game crashed at address 0. The host now answers with the build's version.
- **DLC menu:** "Purchase" calls `nn::ec::ShowShopAddOnContentList`, which opens the eShop applet. The SDK crashed there, since the host runs no applets. The shop now opens and closes at once, and the game reports no add-on content as before (seen in play, 2026-10-29).
- **Saves:** 2.2.1 opens a 1.0.0 save. Its save is 20 MiB instead of 10 MiB, and a save written by 2.2.1 may not load in 1.0.0.

Verified (Apple M5, macOS 27.0.1, 2.2.1, a copy of the 1.0.0 save):

| Check | Result |
|---|---|
| ExeFS, update Program NCA, merged RomFS (all six IVFC levels) | pass, pinned |
| `structck.py`, `equiv.py --min-eff 8` | OK for all three modules; 0 divergent sites; 300 of 300 mutations caught |
| Boot with A presses | title screen "Ver. 2.2.1", Continue loads the 1.0.0 save, a battle in Everblight Plain renders; 230 s with no crash |
| Played by hand | works (in play); "Purchase" in the DLC menu crashed the game, and after the fix it returns to the menu |

### 2026-10-29: stage 7, the app

`make app` builds `Xenoblade Chronicles 3.app` from the work folder, with the launcher copied from ot0. Changes for this game:
- The frame rate choice is gone. Ot0 needed it for its Unreal tick-rate patch, but this game holds itself to 30 fps and has no unlock. The app and `make boot` now both run with `TOTK_UNCAPPED=0`, so frames follow the game's own present interval on any display.
- TOTK's shortcut keys 2–4 are off. The first session played in the app showed cutscenes enlarged and shifted to the top-left corner. The log showed key 2 had turned on MetalFX frame interpolation, and interpolated frames are shown without the game's crop: the game draws its cutscenes at 1280×720 in the corner of a 1920×1080 picture. Keys 3 and 4 (2K modes, ultrawide) had nothing behind them for this game. The in-game help now lists key 1 only.
- The system fonts were found through absolute paths into the work folder. They are now relative to the folder the runtime runs from (the work folder, or the app's `game` folder), and `make app` bundles them. The game has not asked for them so far.

Verified (Apple M5, macOS 27.0.1, 1.0.0):

| Check | Result |
|---|---|
| `make app` with the game's icon | builds in about 30 s; `codesign --verify --deep --strict` passes, also after three runs |
| Work folder renamed, game started from inside the app with the launcher's settings (MetalFX on, 1920×1080 window) | boots, difficulty selection renders, 150 s with no crash |
| First run with an empty shader cache | one loading screen waits about 50 s for 92 pipelines and 10 libraries to compile |
| Second run | no wait; 30 fps |
| Started from Finder | works (in play): settings window, then the game through the opening |
| Cutscene after pressing 2 | enlarged into the top-left corner until restart; fixed by turning keys 2–4 off |

### 2026-10-29: stage 6, played by hand; save and load

The game was played by hand through `make boot` (two fixes to the target: it opened no window and submitted no GPU work). They walked and fought in Everblight Plain, saved, quit, started again and loaded the save. All worked. Rare, brief screen tearing or thin black lines remain.

Verified (Apple M5, macOS 27.0.1, 1.0.0, Everblight Plain):

| Check | Result |
|---|---|
| Walking, camera and battle with keyboard or controller | works (in play) |
| Save, quit, start again, load | the save loads at the same place (in play) |
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
| Sound | working (by ear, in play) |
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
