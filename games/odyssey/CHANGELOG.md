# Super Mario Odyssey: changelog

## Status

What works and what does not: [README.md](README.md). This page says what changed, why, and how each change was
measured. The development record before this page is the "Development log" in the README.

## Where the changes come from

Everything below was first built and played in a Super Mario Odyssey 1.4.1 port that is not in this repository (a copy
of `games/totk` with the SDK 7.3.2 branches Breath of the Wild uses; `games/ssbu` began as a copy of it too). Each part
was then brought into `games/odyssey`'s own runtime and graphics layer, adapted where this folder differs, and measured
again in the packaged `Odyssey.app`. No file outside `games/odyssey` is read.

All measurements on 26 October 2026: MacBook Pro, Apple M1 Max, 64 GB, macOS 27.0.1; the app built with
`scripts/build_odyssey.py` from a base NSP and the 1.4.1 update NSP, a save in the Wooded Kingdom.

## Log

### 2026-10-26: update 1.4.1, from a base NSP and an update NSP

- `scripts/merge_nsp.py` joins a base NSP and an update NSP into one NSP (PFS0), every entry copied unchanged; a name in
  both must hold the same bytes. `build_odyssey.py --update <update NSP>` does it first (again when either input is
  newer), into `<work>/input/odyssey.nsp`.
- 1.4.1's update RomFS failed in `merge_romfs.py` with "AES-CTR-Ex table must cover the section up to itself": its
  AES-CTR-Ex table covers only the data before the indirect table, which is then plain AES-CTR too. The table's extent
  picks the layout; a wrong choice fails the bucket checks or the IVFC hashes, and all six IVFC levels verified.
- The merged RomFS from the base and 1.4.1 is 4,972 files, 5,678,168,236 bytes, with the SHA-256 that
  `assets/expected-local-update.json` pins for 1.3.0; step 5 verifies ExeFS, rewrite and RomFS against the pins as for
  1.3.0. `inspect_version.py` and `extract_app_icon.py` accept 1.4.1.
- The icon step refused a work folder outside the repository (`--work` elsewhere); it now allows one, or a `*_build`
  folder of `games/odyssey`, as before.
- The window title said "Ver. 1.3.0" for a 1.4.1 build: the two share their executables, so the runtime cannot tell
  them apart. `extract_app_icon.py` writes the version from the update's Control data, the app carries it and the
  launcher passes it on (`ODYSSEY_GAME_VERSION`). The title now reads "Ver. 1.4.1".

### 2026-10-26: the tick in the music

The music is an ADPCM stream whose buffers each carry the end-of-stream flag. The renderer and the mixer's buffer walk
stopped the 240-sample frame at such a buffer, so the rest of the frame stayed silent and the next buffer began with the
next frame: a short gap at every buffer join, heard as a steady tick. yuzu (DecodeFromWaveBuffers / EndWaveBuffer) and
Ryujinx (DataSourceHelper) only restart the played sample count at end of stream and go on with the next buffer in the
same frame; `audio_renderer.c` (advance_voice, skip_empty) and `audio_mixer.c` (walk_step, walk_skip_empty) do too.

- Measured with a recording of the mixed output (`TOTK_AUDIO_CAPTURE`, 30 s on the title screen) and the clicks in it
  sorted by their place in the 240-sample frame: before, 85 clicks, 63 of them at two fixed places (offset 0..19 and
  160..179); after, 30 clicks spread over the frame (the music's own), the same loudness. A step injected at a frame
  boundary every 0.25 s is found in both recordings.
- `tests/audio_routing/probe.c` eos_samples (the samples after an end-of-stream buffer come from the next one in the same
  frame) fails with the old mixer ("sample 100: 0 != -0.5"); `probe-audio` section 8 checks the voice's position and
  played count across the boundary.

### 2026-10-26: keys 6 and 7, controller type and mouse mode

`games/totk` has key 6 (controller type: auto, Xbox, PlayStation, Switch) and key 7 (mouse mode: camera, gyro aiming,
mouse buttons); Odyssey's copy of the input code did not. `gamepad.m`, `gamepad_layout.h`, `controller_map.h`,
`motion_fusion.c` and the new `mouse_look.h` take both (and the generic USB HID pads); the HID path passes Odyssey's stick
reach like the GameController path. `frontend.m` holds the pointer in mouse mode and lets it go on Command-Tab, during
text entry and on close, keeping Odyssey's own cursor and display-sleep handling.

- `tests/test_input.py` runs the four input probes; with the old `gamepad.m` the controller-type and mouse-mode probes
  fail. In the app: "controller type: xbox (key 6)", then "playstation"; "mouse mode on", "mouse mode off".

### 2026-10-26: key 5, edge smoothing

`games/totk`'s MetalFX temporal anti-aliasing (`totk_taa.m`): a temporal scaler at 1:1 on the linear HDR scene just
before the compose draw samples it, the G-buffer geometry jittered by a sub-pixel offset each frame, motion from the
camera. Off by default; key 5 or `TOTK_TAA=1`.

- Odyssey's compose draw is fragment shader 1068aa25 (`TAA_COMPOSE`, `nvn_obj.h`); its scene is a one-slice 2D array,
  which MetalFX gets as a 2D view. The jitter leaves out full-screen draws, moved or squeezed UI, occlusion-counted draws
  and draws that sample an attachment of their pass.
- The camera cut is Odyssey's (`odyssey_mfx_cut`): Odyssey's positions are centimetres, and TOTK's 8 units are ordinary
  camera motion there. In the first 30 s after Resume the history was reset 124 times with TOTK's cut and 3 times with
  Odyssey's.
- In the app: "temporal scaler 1600x900, scene format 92, output RGBA16Float, 1:1 with jitter", 5,184 frames
  anti-aliased by frame 14,400, 0 without depth or camera, 60 fps. A game frame with key 5 off / on: mean absolute
  Laplacian 10.79 -> 9.31; the railing's diagonals lose their steps, the HUD is unchanged.

### 2026-10-26: key 4, picture shape

Key 4 was a stub. It now cycles 16:9 -> fitted to the window -> 21:9 -> 16:9 (`aspect_math.h`; `TOTK_ASPECT` names the
wide ratio or starts on a step). Odyssey gets no code patch: every camera projection is a sead::PerspectiveProjection
(vtable main+0x13540f8, the layout Breath of the Wild 1.9.0 has), and `runtime/odyssey_aspect.c` writes the ratio into the
16:9 ones at every present, found by a background thread that copies the guest heap through the kernel
(`mach_vm_read_overwrite`). The graphics layer's present stretch and HUD squeeze do the rest.

- In the app: "4 16:9 camera projections found (scan 2, 1494 ms)"; fitted to a 1774x976 window (1.82:1) the picture
  fills it, 21:9 widens the view with the HUD kept at 16:9, 16:9 is the original again; 60 fps in each.

### 2026-10-26: key 3, the 3D scene up to 2560x1440

Key 3 was a stub. With a bigger render size (`build_odyssey.py --render-res`, default 2560x1440) it cycles the shipped
1600x900 -> 1600x900 with MetalFX to the window -> native 1920x1080 -> native 2560x1440 while playing; the game starts as
shipped. Built with `--render-res 1600x900`, key 3 turns MetalFX to the window on and off.

- `runtime/game_patches.c` sets the docked size (window, swapchain textures and window crop: six instructions, each
  checked before any is written), grows the render target pool, its heap and the game's memory heap with the scene area
  (the shipped 100 MiB pool holds a 2112x1188 scene, not 2176x1224), and replaces the scene size pick (main+0x5efc20) with
  one that holds the result under key 3's cap.
- `scripts/make_render_res_overlay.py` scales the Console steps and SnapShot of the game's graphics table
  (SystemData/ProjectGraphicsQuality.szs) into a RomFS overlay with `render-res.txt`, so the code sizes and the table
  agree: raising only the scene size in code drew light in blocks on the ground, because buffers sized for the largest
  step stayed 1600x900.
- In the app: "render target pool 160 MiB (shipped 100), its heap 230 MiB (shipped 170)", "application pool +60 MiB";
  key 3 logged each state with the scene size the game then drew (1600x900, 1920x1080, 2560x1440). Game frames at the
  three sizes: mean absolute Laplacian 4.42 / 5.19 / 8.76; 60 fps at 2560x1440.

### 2026-10-26: the stutter every 64th frame that grows with play time

The graphics layer's sweep of cached texture-type views (`type_views_sweep_retired`, every 64th submission) walks every
cached view, and a view only left the cache once its parent allocation left residency. Odyssey rebuilds the sampling
view of one resident 1600x900 2D-array render target twice a frame, so every frame added keys: in the port these changes
come from, 28,000 entries after four minutes, a sweep of 12 ms every 64th frame after five minutes and about 200 ms after
an hour. A view no draw asked for during the last 64 submissions now leaves the cache (deferred until the GPU is done
with it), and a `[type-views]` line about once a minute shows the cache size, views made and retired, and the sweep time.
These are the `games/odyssey` lines of #791, which makes the same change in all three copies of `nvn_mtl.m`.

- In the app (1.4.1, six minutes standing in the Wooded Kingdom after Resume, 24,860 frames): the `[type-views]` line
  showed 130 cached views every minute while the views made rose from 2,402 to 40,802 (about two a frame) and 40,672
  left the cache; the sweep took 0.11 to 0.27 ms. With the old code a view left only with its allocation, so those
  views would have stayed. The longest frame per 300 after loading was 25 to 30 ms, flat to the end; 0 FATAL.

### 2026-10-27: the settings window names the update and offers 2560 × 1440

The settings window (#910) said "Version 1.3.0" for every build, and its resolution list stopped at 2240 × 1260, because
2400 × 1350 and 2560 × 1440 had stopped the game in its SDK. Since #1076 a picked resolution goes through key 3's render
size patch, which grows the game's memory heap together with the render target pool, so 2560 × 1440 runs too.

- `scripts/odyssey_launcher_ui.m` reads the version from `Resources/game-version.txt` (written from the update's Control
  data; 1.3.0 and 1.4.1 share their executables). The resolution list, `odyssey_gfx.h` and `package_native_app.py` (the
  quality table made at packaging) take 2560 × 1440.
- In the app (1.4.1, a copy of a save in the Wooded Kingdom): the window shows "Version 1.4.1" and 2560 × 1440; picked,
  the log says "docked render size 2560x1440 (from TOTK_RENDER_RES)", "render target pool 160 MiB", "memory heap
  0xc6c00000", the game starts with "3D scene 2560x1440", and key 3 steps 1600×900 → MetalFX → 1920×1080 → 2560×1440.
  Game frames at those steps: mean absolute Laplacian 9.24 (start) / 4.79 / 4.73 / 5.59 / 9.39. 60 fps (p99 16.67 ms
  in all 42 windows of 300 frames), 0 FATAL.
- The overlay's "Picture (set in the window before the game)" block showed "Resolution: 2560x1440" while key 3 had the
  picture at 1600x900, which read as the picture now. It says "Resolution at launch: 2560x1440 (key 3 changes the picture
  while playing)"; key 3's line above it is the picture now. (`odyssey_gfx_present.inc` is included by `nvn_mtl.m`, which
  the Makefile does not list as depending on it: after editing it, touch `nvn_mtl.m` or the build keeps the old object.)
- With the upscaler on MetalFX, the same block said "Upscaler: MetalFX (off in this build: bilinear)" whenever key 3 was
  not at its MetalFX step, as if the build lacked MetalFX. MetalFX upscales only at key 3's "MetalFX to the window"; the
  line now says "(runs only at key 3's "MetalFX to the window"; now bilinear)". In the app: at key 3's "original
  1600×900" the line carries that note, at "1600×900, MetalFX to the window" it reads "Upscaler: MetalFX" and the HUD
  says "MetalFX on".

### Not brought over

- A reminder to rebuild once enough new shader notes have collected. In the port it comes from, a rebuild took the
  player's notes into the app; `build_odyssey.py` does not, and Odyssey keeps every shader it has met in the player's
  own shader cache ("Odyssey Data/shader-cache", 92,893 files after 26 October's runs), which a rebuild leaves as it
  is, so a rebuild would not help. All of 26 October's runs (a 20-minute session of play and the test runs, mostly in
  the Wooded Kingdom) noted 6 new shaders.
