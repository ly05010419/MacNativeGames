# Breath of the Wild 1.9.0 port: test record

Breath of the Wild (BOTW) 1.9.0 with both DLC packs builds and runs on the BOTW 1.6.0 port of this repository. Both
versions use the runtime and the NVN-to-Metal layer of Tears of the Kingdom (TOTK) in `games/totk`. The BOTW pins and
shader list are in `games/botw/assets`. Code for both BOTW versions is behind `GAME_BOTW`, code for one version behind
`GAME_BOTW160` or `GAME_BOTW190`.

Mac only: Apple M1 Pro, 16 GB, macOS 27.0, built-in 120 Hz Liquid Retina XDR display (3456x2234), 2026-10-20 and
2026-10-21. Each section names its build and settings. The last build tested in game is `3801224` (origin/main
`610d735` merged). The build hashes name commits of the development branch, which this PR adds as one commit. "Not
tested and known issues" lists what this record does not cover.

## Build

Inputs: the base game NSP (`01007EF00011E000`) as the game file, the 1.9.0 update NSP (`01007EF00011E800`) with
`--update`, and `--firmware` (an unpacked firmware NCA folder) or `--shared-fonts` for the six shared fonts, which an
NSP does not hold. Each DLC NSP (`01007EF00011F001`, `01007EF00011F002`) is optional and goes on its own `--dlc`. A
pack that is left out is removed from the work folder, so the app holds the given packs only.

```sh
python3.14 build_app.py ~/Games/botw-base.nsp -k ~/keys/prod.keys --update ~/Games/botw-1.9.0.nsp \
    --dlc ~/Games/botw-dlc-1.nsp --dlc ~/Games/botw-dlc-2.nsp --firmware ~/Games/firmware
```

`build_app.py` picks the game by the title IDs in the file, never by its name. A card image builds 1.6.0. A base NSP
needs `--update`: with the 1.9.0 update it builds 1.9.0, with the 1.6.0 update 1.6.0 (by the update's verified ExeFS
hashes). A package on the wrong option stops the build and names the option it belongs on. `--dlc` with a 1.6.0 card
image or with TOTK also stops the build. A build with a separate update (1.9.0 or 1.6.0) takes its DLC packs from `--dlc` only. Both BOTW versions use
the work folder `BotW_build`, the app `Breath of the Wild.app` and the data folder
`~/Library/Application Support/MacNativeGames/BotW`.

- Code: `games/botw/scripts/extract_exefs.py` (upstream #117) takes the ExeFS from the update NSP. The rewriter is
  unchanged: its generic jump-table detector already rewrites the three 1.9.0 modules to the pinned bytes.
- Data: `games/botw/scripts/extract_romfs.py --update` (upstream #117) merges the base and 1.9.0 RomFS (hactool BKTR),
  verifies every IVFC level and gives the pinned image in 206 s. `games/botw/scripts/extract_dlc.py` verifies all six
  IVFC levels of each pack's Data NCA (`verify_ivfc`, split out of `extract_romfs.py`). It writes
  `extracted/botw190/aoc/<index>`, the layout of the 1.6.0 card that `prepare_romfs_backend.py --aoc` and
  `runtime/aoc_romfs.c` already read.
- Runtime: `make runtime-botw190` in `games/totk` compiles the 1.6.0 runtime sources with `-DGAME_BOTW=1
  -DGAME_BOTW190=1`, and its own graphics objects in `build/gfx-botw190`.

Pins (`games/botw/assets/expected-v1.9.0.json`, from a real run):

| Item | Pin |
|---|---|
| Version | 1.9.0 (1114112) |
| ExeFS | `main`, `main.npdm`, `rtld`, `sdk`, `subsdk0` |
| Module IDs | `main` `cd57b23f…`, `sdk` `574284b4…`, `subsdk0` `f39f4c43…` |
| Rewritten modules | `main` `6d48bff1…`, `sdk` `4d625d9b…`, `subsdk0` `03229589…` |
| Merged RomFS | 15,132,662,100 bytes, 41,062 files |
| DLC `01007EF00011F001` | 1,390,370,680 bytes, 7,094 files: the DLC content |
| DLC `01007EF00011F002` | 648 bytes, one file (`System/AocVersion.txt`): a version and entitlement pack |
| Shared fonts | firmware 22.5.0; four of the six differ from the TOTK 1.0.0 pins. The build checks them as valid BFTTF fonts, as for TOTK, and says that they come from another firmware version |

The two DLC RomFS images are the same as on the 1.6.0 card and have the same pins.

## Rewrite verification

`verify_rewrites.py --version botw190` passed in 53 s and caught 600 of 600 negative controls.

| Module | Structure sites | Semantic sites |
|---|---|---|
| `main` | 12,529 | 12,435 |
| `sdk` | 15,797 | 15,696 |
| `subsdk0` | 9,784 | 9,784 |

1.9.0 has no x18 exit window, as 1.6.0. Its four exclusive stores with a w18 status (one in `main`, three in `sdk`) are
compare-and-swap loops that retry inside the window. `prepare_runtime.py`, `prepare_romfs_backend.py` and
`test_exclusive_windows.py` take `botw190`.

## Runtime: what differs from 1.6.0

1.9.0 has the game code of 1.6.0 with the SDK 20.5 of a later build. The runtime answers the differences:

- `game_version.h` has a `GAME_BOTW190` block: folder `botw190`, and the `nnMain` and `ShowKeyboard` addresses of 1.9.0.
- `nn::init::Start` takes five arguments (SDK 20.5, as TOTK), not the four of SDK 7. NVN reports version 55.15, the
  TOTK value, not 53.311.
- Three calls that 1.6.0 never makes now have answers. `nn::account::PopOpenUsers` gives the one local user.
  `nn::pl::IsRunningOnOunce` answers no, so the game takes the paths of the original Switch. Audio renderer commands 12
  and 13 keep the voice-drop parameter and give it back. The mixer never drops voices.
- Real time at any frame rate (`game_patches_botw.c`, the `#else` branch of `GAME_BOTW160`). Four parts follow the
  measured frame step: the VFR ratio (`ksys::VFR::calc`, main+0x151d484/48c), the fade (main+0x125f118/11c/128), the UI
  animation step (main+0x1280ecc/ee4) and the controller's held counts (main+0xeb0f78/80). Physics keeps six substeps a
  frame at any frame rate (main+0x128106c `fmov s9, #1.0`), as on 1.6.0. 1.9.0 counts held frames in floats, so the
  ratio is the step and no tick is needed. The fast-repeat countdown of the pause menu already reads the VFR's delta
  frames. Each patch checks the original words first. `verify_realtime_patch.py` and `verify_pad_repeat_patch.py` with
  `--version botw190` run the words of the C file in Unicorn, with negative controls, for the UI step and the physics
  substeps too. In one game run, uncapped play (menus about 118 FPS, Great Plateau 69-76 FPS) and key 2 at 30+30
  (29-30 FPS) gave the same speed, within one 30 fps frame, for the inventory tab animations (R, L), the typed item
  text and the jump poses (X, 100 ms steps). A frame-driven animation would be 4 times slower. At 30 fps the inventory
  opens about 80-120 ms later (about 3 frames), then runs at the same rate.
- Docked play renders at 1920x1080, as upstream's 1.6.0 port does: the display-mode routine main+0x1266074, `movk` at
  main+0x12660c8. `TOTK_RENDER_RES=900` keeps 1600x900.
- While a movie plays (movie player instance main+0x1e565a8), the pacing holds 30 fps. Every movie frame is shown
  (`MOVIE_PACE_DEFAULT` 0 for both BOTW versions, upstream's value). The first tower's movie plays at 31.7 fps, and the
  game goes on after it.
- MetalFX keys 2 and 3: 1.9.0 upscales the 1600x900 picture into the 1920x1080 canvas with its own fragment shader
  (`bd7ffdd4`, nine texel reads). `BOTW_MFXUP_KEY` in `nvn_obj.h` is the upscale key of the version that is built. In
  docked play at 1920x1080 that draw is no upscale, so key 3 upscales the window output only.
- Key 5 (MetalFX temporal anti-aliasing): the scaler runs before 1.9.0's compose draw `f7ee6fe1` (tone mapping). Its t0
  is the R11G11B10F scene and t1 the 320x180 bloom. The scene is 1920x1080 in docked play, 1600x900 before that patch.
  `TAA_COMPOSE` in `nvn_obj.h` holds the compose key of each game. The jitter and the camera block need no change.
  Upstream's #157 picks the jittered draws by what they do, not by a shader key, so it acts on BOTW too. 1.6.0's
  compose key is not known, so key 5 has no effect there.

Two changes act on both BOTW versions:

- Write-back staging: the game invalidates 1x1 and 3x3 GPU-written textures every frame. The write-back waited for all
  GPU work (`mtl_finish_gpu`) and made one blit round trip per texture on the game thread. On an earlier build of this
  port, Kakariko ran at 49 FPS. Now the submission that renders such a texture (256 KiB or less) copies it into a
  shared stage buffer at its end. The invalidate waits only for the fences of the listed renderings and swizzles a
  staged texture from its stage. Larger textures keep the read-back blit. With staging, Kakariko holds 59.5 FPS at the
  60 FPS cap. A timer in a test build measured the stage copies at 107 us per game frame (103-113 us, 14 copies and
  4.7 new buffers a frame). That is 1.3 % of a 120 FPS frame.
- Grass masks (after f1043e5): the game flushes its 512x512 grass density mask right after rendering it and reads it on
  the CPU to choose the grass cells 50-130 m away. The flush write-back waited about 3.7 ms a frame for that rendering.
  A target the flush has had to wait for 120 times (1 MiB at most) now lands in memory when its submission completes,
  and the flush no longer waits: the CPU reads the last completed mask, as on hardware, where the game does not wait for
  it either. This also learns the 64x64 light-probe cube maps. Great Plateau at the 120 FPS ceiling: 114-120 FPS and p99
  11.7-15.6 ms before, 119.6-120 FPS and p99 10.6-11.0 ms after. `TOTK_BOTW_LATE_WB=0` restores the waiting write-back.
  `probe-gpu-writeback` covers the landing, neighbour and own CPU writes on the shared edge pages, and a short pool.
- 120 Hz displays: the uncapped mode stays off on a display of 120 Hz or more (`UNCAPPED_HZ`). The BOTW rule that
  ignores the game's present interval 2 acted only in that mode, so BOTW ran at 60 FPS on a 120 Hz display. The
  display-following path now ignores interval 2 in the same cases: real time, `DM_PRESENT_HZ` unset, `TOTK_UNCAPPED`,
  `TOTK_FPS60` and `TOTK_UNCAPPED_INTERVAL` not 0, and no deterministic clock. Interpolation at half rate or 30+30
  keeps its own pacing. The result is one game frame per refresh, up to 120 FPS. The log names the opt-out.
  - Replaced after the merge (user 2026-10-22): the uncapped mode applies on any display, so this rule is gone. Its
    clock's ceiling went to 240, then back to 120 by default the same day (user; `TOTK_UNCAPPED_HZ` still allows up to 240). The real-time frame step lost its 1/120 s floor (TOTK and BOTW), which made a game
    above 120 fps run fast; the Unicorn checks of both versions pass at 144, 165, 240 and 360 fps.
  - The display link that shows the uncapped frames keeps a display rate above 120 Hz too, up to 240 (`link_range` in
    `nvn_mtl.m`; TOTK and BOTW). It turned such a rate into 60, so a 144 / 160 Hz display ticked at a divisor of its
    refresh: on a 160 Hz display about 53 frames a second reached the screen, 18.75 / 25 ms apart, while the game ran
    at 105-120 fps. Only an unknown rate (below 30) still falls back to 60.

Write-back staging A/B on `29bc388`, with upstream's #140 and #143 in the build: Kakariko, uncapped, docked 1920x1080
render, warm shader cache v6, still view from 115 s for 20 s, then a 15 s walk. The "off" build used the TOTK hooks.
Still view: 66.8 fps with staging, 55.5 without (p50 / p95 14.4 / 18.7 ms against 17.5 / 23.3 ms, 212 against 706
frames over 16.7 ms). Walk: 55.7 and 49.2 fps. The GPU time p50 is the same (10.3 and 10.2 ms). Without staging, the
game thread waits for all GPU work at each invalidate, and #143 does not change that wait.

## Route measurements

Build: `5ab49a2` (origin/main `f3ad878` merged, with the write-back leak fix of "Long run and the sinking legs"). Both
DLC packs, the uncapped real-time default, MetalFX keys off, write-back staging on, 1920x1080 canvas, 1600x900 render.
Each route used its own profile with the route save in slot 0, a warm shader cache of upstream's translation chain v5
and one warm-up run. Still view from T0 to T0+20 s (T0 = 115 s), then a walk with a right turn to T0+35 s. FPS is the
frames in the window divided by its length. The GPU column gives the GPU time p50.

| Route | Still FPS | Still p50 / p95 ms | Still frames > 16.7 ms | Walk FPS | Walk p50 / p95 ms | Walk frames > 16.7 ms | GPU ms p50 still / walk |
|---|---|---|---|---|---|---|---|
| Shrine of Resurrection | 77.5 | 12.5 / 15.2 | 30 | 81.4 | 11.9 / 15.5 | 22 | 7.5 / 7.5 |
| Great Plateau (cliff) | 67.6 | 14.3 / 17.0 | 96 | 64.7 | 14.8 / 19.2 | 166 | 8.6 / 9.1 |
| Kakariko (Goddess statue) | 64.0 | 15.1 / 18.1 | 228 | 58.6 | 16.0 / 20.5 | 334 | 10.0 / 10.8 |
| Central Hyrule field | 77.3 | 12.0 / 15.4 | 29 | 75.5 | 12.6 / 16.4 | 51 | 8.3 / 8.3 |
| Korok Forest | 66.7 | 14.6 / 17.3 | 126 | 61.0 | 15.7 / 19.2 | 255 | 10.2 / 11.1 |
| Hyrule Castle exit | 58.4 | 16.7 / 19.0 | 604 | 61.9 | 15.4 / 20.1 | 281 | 11.5 / 9.5 |

- The walk windows include no first-time pipeline compiles. On build `baa4fba`, before the `f3ad878` merge, they did.
  Walk FPS then and now: Shrine 27.9 and 81.4, Kakariko 19.1 and 58.6, Great Plateau 46.5 and 64.7.
- The warm-up runs were within a few FPS of the measured runs, for example the field at 82.4 still and 77.3 walking,
  and the castle at 57.0 still.
- The Kakariko walk also swims into the pond. The shots at 125 s are correct. Those of Kakariko, the field and the
  castle show the banner of a DLC "EX Treasure" quest in the still window, as fresh profiles did before.
- All 12 runs (6 warm-ups, 6 measured) exited with code 0, with no abort. CPU per frame (`nvn_ms` p50): 7.1-10.4 ms.

Other checks, on a build before the `f3ad878` merge:

- Real-time step at 120 FPS (title screen): 3601 frames in 30.0 s. The steps add up to 1.0021-1.0033 of the frames'
  time, so game time runs 0.2-0.3 % fast at the 1/120 s clamp. In game at 78-83 FPS: 1.0002-1.0014.
- Key 3 (Great Plateau): MetalFX replaced 1,801 of 1,801 upscales in a stats window and kept none. The picture is
  correct with the HUD on top, and the still view runs at 65.8 FPS. Key 2 (`interpmax`) interpolates every in-game
  frame, with 7.1 ms of MetalFX GPU time.
- Picture: the pond water colour, the caption picture of a new autosave and the first Game Over glyphs are correct.
- Save loads: the 6 route runs, the 3 warm-ups and the 6 checks each loaded a save, with no stop on a command handle.

## Key 2 modes, rumble and gyro, bloom (2026-10-21)

Build: `7cda4bd`. Built-in 120 Hz display, the Great Plateau route save, shader cache v6, `TOTK_RUMBLE=1 TOTK_GYRO=1`,
no controller connected. One run: Link stands still, and key 2 moves to the next mode every 30 s. Shown per second
comes from the log's present-pacing lines (600 presents per line), interpolated counts from the `[mfx]` lines. The
run exited with code 0 and no abort.

| Key 2 mode | Game FPS | Shown per second | Interpolated frames | MetalFX GPU p50 / p95 ms |
|---|---|---|---|---|
| Off | 68.9 | about 67 | none | - |
| 30+30 | 29.9 | about 60 | 296 of 300 game frames | 0.06 / 0.42 |
| Half the refresh rate | 58.4 | about 115 | about one per game frame (306 of 600 presents) | 0.4 / 7.6 |
| Unlimited | 61.1 | about 115 | about one per game frame (307 of 600 presents) | 0.4 / 7.6 |

- At about 61 game FPS on a 120 Hz display, the unlimited mode acts like the half-rate mode: each game frame gets one
  frame in between.
- In the last 8 s of the unlimited window, Link's idle camera hid the HUD. Interpolation then stopped: 408 game frames
  were not interpolated, with "scene input missing". The scene copy is taken before the first UI draw, so with no UI
  draw there is no scene copy.
- Load sequence: the title opens the "Downloaded DLC" page by itself, so B at 74 s and 77 s closes it before the A
  presses at 82, 86 and 90 s.

Key 2 with no UI, fixed for BOTW by `75cadf2` (`BOTW_MFX_NO_UI_SCENE`, empty for TOTK): in a frame with no UI, the
first draw to the screen after the upscale takes the whole canvas as the scene, and the UI layer is cleared. In three
Great Plateau runs, frames with the HUD are unchanged (2696 of 2700 interpolated, no smear), and the new path ran on
only 2 to 9 frames per run. A re-test at `6aa1821` (two runs, 30+30, Link idle) had HUD hides of 15 to 30 s with fewer
than 100 frames with no UI each. Over the hides, 2990 of 3000 and 3285 of 3300 game frames were interpolated (2988
and 3283 in the second run): 4 to 5 frames lost per hide, against 408 before. The shots show no stale HUD and no smear.

Rumble and gyro (`runtime/hid_feedback.c`, `gamepad_feedback.m`, `motion_fusion.c`, no `GAME_BOTW` guard): 1.9.0's
main module imports `nn::hid::GetVibrationDeviceHandles`, `InitializeVibrationDevice`, `SendVibrationValue`,
`GetSixAxisSensorHandles`, `StartSixAxisSensor` and `GetSixAxisSensorStates`. Its sdk module defines the eleven
`nn::hid::detail` functions that `hid_feedback.c` answers. In game, the log shows `[hid] the game started the six-axis
sensor (handle 0x20003)` (full-key style, device 2), `[hid] the game initialized its vibration devices (handle 0x3)`
and the neutral `[hid] first vibration value from the game: left 0.00@160 Hz 0.00@320 Hz`. So BOTW 1.9.0 uses the
shared rumble and motion path. The "seven-axis sensor: not present" line is the console's own sensor (Labo VR,
`input.c`), a different device.

Bloom 1/4 (`DM_NVN_BLOOM_Q`, on by default, `nvn_mtl.m`) keys on four TOTK fragment shaders. On TOTK it draws the
bright pass straight into the bloom's second level and skips the upsample back into the larger first level. In a
one-frame pass dump of 1.9.0 on the plateau, none of the four keys appears. BOTW copies the 1600x900 scene to 640x360,
the bright pass writes 320x180, and a mip chain goes down to 20x11 and back up to 320x180. The composite reads that
level directly, so the TOTK saving has no BOTW counterpart. No per-version keys were added.

## Key 5, temporal anti-aliasing (2026-10-21)

Build: `78d5f42`, before the docked 1080p patch (scene 1600x900). Still view: 20 s after the load. Walk: a straight run
with a right turn. "Checks on the merged build" has key 5 at 1920x1080.

| Route | Still FPS, key 5 off / on | Walk FPS, key 5 off / on | GPU ms p50, off / on |
|---|---|---|---|
| Kakariko | 64.0 / 56.6 | 11.0 / 16.4 | 10.3 / 12.5 |
| Great Plateau | 67.7 / 58.6 | 54.0 / 57.2 | 8.6 / 11.2 |

- With key 5 on, the log shows "temporal scaler 1600x900, scene format 92", 1454 to 3254 anti-aliased frames, one
  history reset and no frame without depth or camera.
- The Kakariko walks have streaming stalls in both modes, so they do not compare the modes.
- Picture: edges are smoother (Link's hair, rock tops, fence diagonals) and slightly softer. The HUD is correct. No
  ghosting was seen while running. Shimmer over time was not measured (screenshots only).

## Key 4, ultrawide (2026-10-21)

The main camera is a `sead::PerspectiveProjection` at +0x38 in a singleton (pointer main+0x1e39d10, made at
main+0x10ed664, vtable main+0x1d05380, aspect at +0xb0, dirty flag at +8). The game sets the aspect once and rebuilds
the matrix every frame. So while key 4 is on, the present writes the wide ratio into the field and marks it dirty. Only
a 16:9 value is replaced, and another class stops the game. The values come from in-game diagnostics: a scan of live
projections by their vtable, a pointer scan of main's data, and a 25 s write watch that saw no writes in play.

The present stretches the 16:9 canvas over the window. The UI is squeezed back to the middle 16:9 (`UW_UI_BEGINS`,
`UW_NOT_UI_DRAW` in `nvn_obj.h`). Every 6-index quad into the canvas is squeezed, except quads that sample a
GPU-rendered 16:9 picture at least 320 pixels wide (the menus' blurred game picture). Full-screen triangles keep the
full width.

Upstream's key 4 (#120) cycles 16:9, fit to the window and 21:9 on TOTK. On 1.9.0, key 4 switches 16:9 and the wide
ratio only (`TOTK_ASPECT`, default 21:9): `runtime_aspect_step` answers 0 or 2, and the window size is not followed. On
1.6.0, `runtime_aspect_view` and `runtime_aspect_step` are stubs.

Tested on the Mac, 2520x1080 window, `TOTK_ASPECT` default 21:9:

- Great Plateau and Kakariko: a wider view with correct proportions, the HUD in the middle.
- The inventory: the UI and Link are correct, and the blurred background is full width. A faint edge shows where the
  menu's dim layer ends. The map: 16:9 with black bars.
- `TOTK_ASPECT=21:9` starts wide, and key 4 goes to 16:9 and back. At `6aa1821`, `TOTK_ASPECT=display` stops at the
  start with the parser's message.
- Kakariko standing still, one run each: key 4 off 62.9 fps, on 65.0, off 64.1. GPU p50 9.75, 9.69 and 9.67 ms: no
  measurable cost.

Not tested: 1.6.0 (key 4 stays unavailable there), iPhone and iPad (Mac only by design), cutscenes, shrines, other
menus (Adventure Log, System, shops), a real ultrawide display in full screen, and long play. A second camera-like
projection stays 16:9. No objects were seen culled at the screen edges in the tested places. Markers that follow
objects in the world are not moved out yet.

## Checks on the merged build

This branch merges upstream up to origin/main `ab4c9d2`. All game runs below used the Great Plateau route save, shader
cache v6, both DLC packs and window output, and exited with code 0 and no abort.

- `3801224` (origin/main `610d735` merged, clean build): with upstream's two pipeline-archive fixes (`110e6c6`,
  `610d735`), the warm-up builds only the pipeline lists, from the archive (4418 pipelines). At 4000 pipelines, all
  4000 came from the archive, and none was compiled. Title about 120 fps (3591 frames in 30.0 s). The save loads, and
  Link stands on the plateau with the HUD. Still view: 69.1 to 71.6 fps in the 30-second real-time lines.
- The branch tip: CI passes (187 tests and the two UBSan tests). TOTK identity against `ab4c9d2`: 68 of 68.
- `29bc388` (origin/main `f08266c` merged, with #120, #140, #143 and #157): title about 120 fps (3592 frames in 30.0 s),
  and the save loads. The log shows "docked render size 1920x1080" and the real-time line with the UI step and six
  physics substeps. Key 2 at 30+30: 296 of 300 game frames interpolated, the game at 28.7 fps. Key 3: the game's
  upscale draw `bd7ffdd4` is no upscale at 1920x1080 and stays (logged with reason 2). MetalFX upscales the output
  instead, 1920x1080 to 3456x1944, 2400 times, GPU p50 1.38 ms. Key 5: "temporal scaler 1920x1080, scene format 92",
  so the compose draw `f7ee6fe1` was found at the new size. The shot is correct, with the HUD on top. The run ended
  before the first count line (every 1800 frames), so it gives no count of anti-aliased frames. Write-back: 10,000
  small targets written back from the submission's own copy (29,509 copies staged).
- origin/main `d92f463` merged, clean build: Great Plateau still view 70.0 fps (p50 13.7 ms, 79 frames over 16.7 ms),
  walk with a right turn 54.1 fps.
- An audit of every `GAME_BOTW` block found no 1.6.0 address or check word that reaches the 1.9.0 build without a
  1.9.0 value. The 30-second real-time line reads a delta time of 33.27 ms at 28.7 fps, so the VFR field +0x138 is
  right on 1.9.0. The 1.9.0 `main.npdm` gives the main thread priority 44, core 0 and cores 0 to 2, as `threads.c`
  assumes (ideal core 0, mask 7).
- A syntax-only compile with `GAME_BOTW160` of the BOTW files, `hle_nvn.c` and `nvn_mtl.m` gives no
  error.

## Decisions: upstream or this port

For each area, the port takes upstream's approach unless its own is clearly better, with evidence.

| Area | Approach | Evidence |
|---|---|---|
| Base game plus a separate update (#117: `843a070`, `1d69872`, `c8c9705`, `da5f7a7`) | Upstream's `games/botw/scripts/extract_exefs.py` and `extract_romfs.py --update`, written for 1.6.0, also import 1.9.0. The TOTK scripts stay upstream's | ExeFS: 5 of 5 files match the 1.9.0 pins. RomFS: the pinned image in 206 s |
| 1.9.0 DLC NSPs | This port's `--dlc` and `extract_dlc.py`. Upstream has none: 1.6.0 takes its packs from the card image | `test_dlc_import.py`. Both packs mount in game |
| Above 30 fps (#103: `0d4a81c`, `52d8d35`) and the real-time step | #103's functions (UI step, six physics substeps, docked 1080p, movie hold) with 1.9.0 addresses (`#ifdef GAME_BOTW160` / `#else`). The real-time step has its own 1.9.0 sites. They need no 1/30 s pad tick, because 1.9.0 counts held frames in floats. 1.6.0 is unchanged | `verify_realtime_patch.py` and `verify_pad_repeat_patch.py`, and the game runs in "Checks on the merged build" |
| Key 4 (#120) | A write into the camera's aspect field at each present and the UI squeeze (`UW_*`), on or off only. `aspect_patch.h` has TOTK sites only. Upstream's `runtime_aspect_step` stays | "Key 4, ultrawide" |
| Small-target write-back (#140) and texel regions on the GPU (#143) | Both stay next to write-back staging: they change other steps of the write-back or other paths. `wb_ent` holds the fields of #140 and of the staging | Kakariko still view with #140 and #143 in the build: 66.8 fps with staging, 55.5 without |
| Build options (#170, #171) and player mods (#172) | #170's font branch and error, and #171's relative shader list. #171 covers 1.9.0: the shader list is the only path that `build_app.py` gives to make. #172's mods are out of scope for 1.9.0: a 1.9.0 app reads `mods/botw190`, which `make_mod_manifest.py` does not take | `test_botw_import.py` and `test_game_selection.py`. `docs/mods.md` names 1.6.0 only |

## Shared edits for the TOTK re-test

Every file under `games/totk` that this port changes (`git diff --stat ab4c9d2 HEAD -- games/totk`):

| File | What changed | Guard |
|---|---|---|
| `Makefile` | new rules `runtime-botw190`, `build/totk-native-botw190`, `build/gfx-botw190/*.o`, `build/botw190/*.h` and `probe-audio-botw190` | additive; no TOTK rule changed |
| `runtime/account.c` | `nn::account::PopOpenUsers` gives the one local user | `#ifdef GAME_BOTW` |
| `runtime/aoc_romfs.c` | default DLC folder `extracted/<TOTK_GAME_DIR>/aoc` instead of the fixed `extracted/botw160/aoc` | the file is inside `#ifdef GAME_BOTW` |
| `runtime/audio_mixer.h` | two fields keep the voice-drop parameter; four more keep the ramp state (`prev_volume`, `ramp_ready`, `was_started`, `last`) | `#ifdef GAME_BOTW` |
| `runtime/audio_mixer.c` | volume ramps and depop (`voice_ramp_mix`, `voice_depop_prepare`, `mix_depop`, `audio_mixer_voice_release`) | `#ifdef GAME_BOTW` |
| `runtime/audio_ramp.h` | new header with the ramp arithmetic | included only under `GAME_BOTW` |
| `runtime/audio_renderer.c` | `IAudioRenderer` commands 12 and 13 (Set/GetVoiceDropParameter); the ramp state is reset with the voice, after the tail of a stopped voice is queued; `probe_ramp` and `probe_ramp_edge` in the audio probe | `#ifdef GAME_BOTW` |
| `runtime/game_patches_botw.c` | The 1.9.0 real-time, UI-step, physics-substep, held-count, docked 1080p, movie-hold and ultrawide-camera sites. The 1.6.0 sites are unchanged. A `runtime_render_res` stub answers 0 for the shared `romfs.c` of `466330d`. Without it, no BOTW binary links | `#ifdef GAME_BOTW160` / `#else`. Only BOTW binaries compile this file |
| `runtime/game_version.h` | the 1.9.0 version block | `#elif defined(GAME_BOTW190)`, before the TOTK blocks |
| `runtime/loader.c` | the five-argument `nn::init::Start` of SDK 20.5; `module_finis` is compiled for 1.9.0 too | `#ifdef GAME_BOTW160` / `#elif defined(GAME_BOTW)`; `#ifndef GAME_BOTW` became `#ifndef GAME_BOTW160` |
| `runtime/shared_font.c` | `nn::pl::IsRunningOnOunce` answers no | `#ifdef GAME_BOTW` |
| `scripts/extract_romfs.py` | the IVFC loop moved into `verify_ivfc` | refactor; same checks, messages and report |
| `scripts/prepare_romfs_backend.py` | `botw190` in `--version` | new choice only |
| `scripts/prepare_runtime.py` | `botw190`: its `main` hash check and the DT_INIT shape of 1.4.3 | new choice; TOTK branches unchanged |
| `scripts/test_exclusive_windows.py` | `'botw190': []` in `WINDOWS` | additive |
| `scripts/verify_rewrites.py` | `botw190` in `--version`; an empty window list passes for it, as for `botw160` | new choice; TOTK checks unchanged |
| `third_party/d2r_gfx/gfx/nvn/hle_nvn.c` | 1.9.0 reports the TOTK NVN version (the `BOTW_FAST_DISPLAY_*` hooks were removed after the merge) | `#if defined(GAME_BOTW) && !defined(GAME_BOTW190)`; the hooks are empty for TOTK |
| `third_party/d2r_gfx/gfx/nvn/nvn_dirty.c` | pools that share a host page with another pool get ranges for their own pages; lookups walk range by range; a pool removes only its own ranges (the 30 fps after the map's Hero's Path playback) | `#if defined(GAME_BOTW)` / `#if !defined(GAME_BOTW)` around whole functions |
| `third_party/d2r_gfx/gfx/nvn/nvn_mtl.m` | `BOTW_REUP_TRACKED` and `BOTW_WB_*` hooks at the ends of existing lines, the staging bodies at the end of the file (their stage read inside an `@autoreleasepool`, `5ab49a2`), `BOTW_MFXUP_KEY` in the scene test; `TAA_COMPOSE(fs)` in key 5's compose test; `UW_UI_BEGINS` and `UW_NOT_UI_DRAW` in key 4's two UI tests; `BOTW_MFX_NO_UI_SCENE` at the end of key 4's UI line (key 2 in frames with no UI); `BOTW_WB_ENT_FIELDS`, `BOTW_WB_STAGE_FREE` and `BOTW_WB_NOTED` next to upstream's small-target copy (`snap`, #140) | bodies in `#ifdef GAME_BOTW`; for TOTK the hooks are empty, `WB_WAIT_GPU` is `mtl_finish_gpu()`, the call it replaces, and `TAA_COMPOSE` is the old `0ac9a549` test |
| `third_party/d2r_gfx/gfx/nvn/nvn_obj.h` | the `BOTW_REUP_TRACKED` and `BOTW_WB_*` macros; `BOTW_MFXUP_KEY` and `TAA_COMPOSE` for each version; `UW_UI_BEGINS` and `UW_NOT_UI_DRAW` (for TOTK, the old expressions); `BOTW_MFX_NO_UI_SCENE` | inside the existing `#ifdef GAME_BOTW` / `#else`, empty in the `#else`; `TAA_COMPOSE` has its own `#if` block |
| `tests/dirty_race/probe.c` | 12 overlap checks for the BOTW tracker | `#if defined(GAME_BOTW)` |

Outside `games/totk`, `build_app.py` adds the 1.9.0 branch, `--dlc` and the package checks (`check_package`, BOTW
only). The font functions are upstream's (#170): a 1.9.0 build checks its fonts against the TOTK pins, as TOTK does
with fonts of another firmware. A TOTK build sees these changes:

- `--dlc` stops the build ("has no DLC support"). The help text names BOTW 1.9.0.
- `identify_game` reads the title IDs of every game file from its tickets (`ticket_title_ids`, `title_kind`), without
  title keys. A TOTK DLC NSP as the game file stops with "add-on content (DLC), not the game".
- The early NSP font check also accepts valid fonts in `BotW_build/totk/extracted/firmware/shared-fonts` next to the
  game file (only without `--work`). A TOTK NSP that passes this way stops with the same message right after the game
  is identified, before any long step.

For TOTK 1.0.0, 1.1.2, 1.2.1 and 1.4.3, the preprocessed code (`clang -E -P`) of every changed C, Objective-C and
header file is identical to origin/main `ab4c9d2` (68 of 68 checks: 13 changed files and the 17 sources that compile
them, 4 versions). The Python scripts keep the TOTK defaults, and the Makefile only adds rules. TOTK itself was not re-tested in game on this branch.

## Long run and the sinking legs

Sinking legs: Link stands idle after a load of the 08:29 Great Plateau autosave, and his legs sink into the stone
path by the ruin wall. A new save at the same spot (System > Save, slot 1) gives the same: five seconds after its load,
the legs sink mid-thigh. With the write-back off (`flag writeback 0`) the legs sank the same, which fits the cause
below: nothing is copied back then.

Cause (2026-10-22): the game renders a 1x1 target on the GPU and reads the result on the CPU with no invalidate over its
memory, as the idle foot placement and the water material loader do. The port copied GPU renderings to memory only at an
invalidate, so the CPU read stale bytes: legs that sink, and water whose colour block stayed zero (the Bottomless Bog
rendered clear and milky). One local fix on the old base 7305d4e (`bedb076`) wrote such 1x1 renderings back at each
present: legs straight at 60, 75 and 90 s, bog dark and opaque; the same build with `TOTK_WB_EAGER=1x1` also kept the
legs straight, without it they sank.

Test on origin/main `daf270e` (M1 Pro, macOS 27, scratch apps built from the worktrees, copied saves, the same inputs;
legs: save slot 5 of the 08:29 autosave, no input after the load; bog: save slot 2, camera turned left and down; frames
at 38 to 90 s after the start):

| Build | Legs | Bog |
|---|---|---|
| origin/main | sunk to the knees at 42 s; a bokoblin that stays hidden in the other runs attacks; Game Over at 60 s | no water: the ground shows through (milky, clear) |
| origin/main with pull request #239 (tiny GPU targets copied to memory when their submission completes) | straight at 42, 60 and 90 s, bokoblin hidden, about 59 fps | dark and opaque water |

So the cause above holds on current main. #239 cures the clear bog water; the straight legs in this row were not repeated:
on #239's head (merged as `eda6340`) Link sank 3 of 3 times (2026-10-24). Pull request #511 (open) fixes the legs. This
branch carries no fix of its own. Not tested: our own write-back fix on current main (it did not run there; it passed on
the old base only), and #239 on 1.6.0.

Leak fix: the first 25-minute run (Shrine route, `f3ad878` merged) stopped at 1509 s at the 5000 MB GPU budget. Its
485,380 write-back stage buffers (2.9 GB) were never freed, because the game thread has no autorelease pool. `5ab49a2`
puts an `@autoreleasepool` around the stage read.

25-minute run after the leak fix, Korok Forest with Link moving: 1630 s, exit code 0, no abort. From 120 s on, the
run walks 10 s each minute, with a 2 s right turn, then presses A. The 13 shots, one every 120 s, show Link moving in
the forest and no Game Over. GPU allocation: 1138 MB at frame 300, 1823 MB at about 170 s, 1916 MB at the end. After
the warm-up this is about 4 MB/min, against 10 MB/min before the fix. Resident memory: 2686 to 2900 MB. A first
moving try on the field route is void, because Link died before 245 s and the run stayed on Game Over.

## Audio: volume ramps and depop (2026-10-22)

The mixer applied a new voice volume or channel mix volume as a step at the 5 ms frame edge, and a stopped or paused
voice dropped to zero at once. BOTW ducks the music when the pause menu or the map opens, and each step on a loud sample
is a click. The Switch DSP ramps the voice volume and each channel's mix volume from the last frame's value and decays a
stopped voice's last samples (Ryujinx.Audio `CommandGenerator.GenerateVoice`: `VolumeRamp`, `MixRamp`,
`DepopPrepare`, `DepopForMixBuffers`, 0.962189 per sample at 48 kHz). `audio_mixer.c` does the same under `GAME_BOTW`;
`TOTK_AUDIO_RAMP=0` gives the old mixer for a comparison.

- `tests/test_audio_ramp.c` (CI, no game data) runs `audio_ramp.h`: a duck from 1 to 0.25, a mix volume that falls to 0
  and a stopped voice's decay may not step a sample by more than 0.04. With `-DAUDIO_RAMP_STEP` it fails (steps 0.6,
  0.4 and 0.5).
- `make probe-audio-botw190` (needs game data): a 0.8 voice ducks and stops, no sample steps more than 0.04. Run on the
  packaged binary of this branch (origin/main `daf270e`): it passes, and with `TOTK_AUDIO_RAMP=0` it fails with "volume
  step or stop is not smoothed".
- The same probe has three edge cases of the depop: a voice that is stopped and then released, or reused by a new voice,
  before the next frame still fades from its last sample, and a voice whose channel was muted for a frame adds nothing
  when it is paused afterwards (its last added sample is 0). Run on a build of origin/main `a988b5e` with this
  branch's runtime: without the fix the release and reuse cases step 0.8 and the mute-then-pause case adds 0.0032; with
  it the probe passes (and fails with `TOTK_AUDIO_RAMP=0`). A voice freed while it is still started is cut. When a new voice starts at once in the place of a stopped one, the old tail still fades and the new voice ramps up from 0 over its first frame, so the two do not add to a clipped step (0.8 plus 0.77 would clip to 1.0) and a silent new voice does not cut the tail to 0.
- Game A/B on the old base `bedb076` (Kakariko save, 7 pause-menu and map cycles, the same inputs, 30 s capture): 4
  clicks (steps 0.15-0.20, about 12 times the local step) with the old mixer, 0 with the fix, 0 underruns in both.
- Game run on origin/main `daf270e` (bog save, 290 s, menu and map cycles from 100 s, 30 s capture): 0 clicks with the fix
  and 0 without, the largest step 0.133 in both captures, 0 underruns, exit 0, about 60 fps. This scene did not repeat the
  clicks of the old base, so it proves no gain; it shows no harm.
- BOTW 1.6.0 (2026-10-24, M1 Pro, macOS 27, base v0 + 1.6.0 update, Great Plateau save, docked): the ramps act, and 5
  pause-menu and map cycles gave 0 clicks with the ramps and 0 with `TOTK_AUDIO_RAMP=0`: no harm, no gain shown.
- Not tested: a listening test by the owner (pause menu, map, shrine, fast travel) and splitter voices (BOTW opens none).
- A run that ends in Game Over has no world sound (digital zero): the first runs of this check, started from the legs
  save on a build without the legs cure, were silent for that reason.

## Not tested and known issues

- Sinking legs after a save loads, and the clear Bottomless Bog: the clear water is cured by #239 (merged), the legs are not (pull request #511, open; see "Long run and the sinking legs").
- GPU allocation grows about 4 MB/min after warm-up (1916 MB after 27 minutes): deferred. At that rate, the 5 GB
  budget is about 13 hours away.
- Key 2 with no UI: fixed by `75cadf2`, with light proof (4 to 5 frames lost per HUD hide, against 408 before). A long
  stretch with no UI was not seen.
- Menu and HUD speed above 30 fps: the same in one run at 30 fps and uncapped (see "Runtime: what differs from
  1.6.0"). Rolling objects, drops, hearts and the minimap were not checked. The held d-pad in the pause menu and the
  sprint landmark were checked in Unicorn only.
- Rumble and motion aiming with a real controller: not tested, because no controller was connected.
- A cold start with an empty shader cache: not tested. Every route run had a warm cache.
- A card image as the game file with a 1.9.0 `--update`: not tested. The build takes the DLC packs from `--dlc` only.
- The iPhone and iPad builds: not built. The write-back staging and the 120 Hz rule also act on 1.6.0, which was built
  and run on 2026-10-24 (see the audio section).
- Keys 4 and 5 do nothing on 1.6.0.
- TOTK: not played on this branch. The evidence is the 68 of 68 preprocessed identity.
- Upstream's `[dead-keep]` set grows linearly (30k to 397k entries in 25 minutes, from `e0ad452`): not changed here,
  because it is upstream's shared code, and a change to it changes TOTK.
- Key 3 native 2K: above 1080p the game fails to make its heaps. It needs a memory patch of its own, and upstream's
  1.6.0 port has none either.
- Upstream's survey tool (`TOTK_SURVEY`, `f4b3a95`) knows the 1.6.0 player chains only. On 1.9.0 it stops with a
  message.
