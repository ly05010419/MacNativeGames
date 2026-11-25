# Ocarina of Time on an Apple M2: measured performance and where the time goes

Measurements of 2026-11-25 on a MacBook Air M2 (10-core GPU, fanless), macOS 27.2.1, internal 2560×1664 @ 60 Hz,
app built from `3b0eae42`. Scene: the newest save, Kokiri Forest, standing still; private copy of the player's
profile and shader cache; frame lock off (`flag fps_lock 0`); 40 s windows after a 35–60 s warm-up, two runs each,
host not idle. Frame times come from `TOTK_BENCH_FRAMES`; GPU/CPU from `metalperftrace` and the in-game 300-frame
windows; pass/dispatch costs from `TOTK_PASS_GPU_FILE` / `OOT_DISPATCH_GPU_FILE` (`flag pass_gpu 30`).

## The headline numbers

| Setting (canvas row) | FPS (median frame) | GPU busy per frame |
|---|---|---|
| 720p native (default) | 34.1 fps (29.2 ms) | ~27.9 ms — the Metal layer is active ~100 % of the time |
| 720p from 480p (handheld 3D) | 39.0 fps (25.6 ms) | — |
| 720p native + the game's own dynamic resolution (`OOT_FIXED_RES=0`, now the panel's 动态分辨率 row) | 39.0–40.1 fps (25.3 ms) | the game lowers the 3D scale itself (to ~683×384 at the heaviest here) |
| 1080p native | ~30 fps (33–34 ms) | — |

**The game is GPU-bound, not CPU-bound, in this scene.** `metalperftrace`: on-GPU time 27.6–28.2 ms per frame
(stddev 1.5 ms) against a 29 ms frame; CPU begin-to-present 21.5 ms, i.e. the CPU mostly waits for the GPU.
Total CPU is only ~1.5 cores of the 10, and the Time Profiler puts ~15 % of samples in the guest's own code,
the rest spread over the encode stage (~8 %) and ~80 guest worker threads.

## Where the GPU time goes (per frame, 720p canvas)

From the pass/dispatch dumps (encoder spans overlap, so the buckets are indicative, not additive):

- ~63 render passes at the 3D size + ~100 smaller ones, ~160 passes and ~1,215 draws per frame in total.
  Main-target passes ≈ 16 ms, half-res (640×360) post ≈ 2.6 ms, shadow cascades (depth-only 1000×1000, 3/frame)
  ≈ 3.3 ms, small 128×128 passes ≈ 2.1 ms.
- ~31 compute passes ≈ 9.8 ms/frame; the five heaviest (mesh / culling shaders) ≈ 5.6 ms of that.
- The keep-alive spinner ≈ 0.7 ms/frame (measured `TOTK_GPU_KEEPALIVE=0`: no change at this load, the GPU never idles).

The cost barely drops when the 3D resolution is halved (720p → 480p: render passes 16.4 → 13.0 ms): the frame is
**draw-call and pass-count bound, not pixel bound** — ~1,200 draws and ~160 passes of fixed per-draw/per-pass cost
plus attachment traffic. That is the Switch 2 mesh-shader pipeline's shape, and it is the main thing to attack for
60 fps on a weaker Mac.

## What was measured and ruled out (2026-11-25, M2, 720p canvas, 40 s A/B windows)

| Candidate | Result |
|---|---|
| GPU keep-alive off (`TOTK_GPU_KEEPALIVE=0`) | neutral (34.5 vs 34.2 fps) — GPU already ~96 % busy |
| Cheaper scheduler yields (`flag yield_fast 2`) | neutral (34.1 vs 34.2) |
| Async replay (`TOTK_ASYNC_REPLAY=1`) | neutral / slightly worse (38.7 vs 39.4 at 480p) |
| Drawable at the game picture's size (`TOTK_DRAWABLE_SCALE=0.5`) | neutral — the present pass is a cheap blit |
| Present link off (`DM_PRESENT_LINK=0`) | neutral at 720p; only matters when the window is occluded |
| Shading-rate images (NVN2 VRS) | **unused by the game in this scene**: ~6,984 binds / 300 frames, every palette entry 1×1. The full-rate shading is not waste here |
| The game's own dynamic resolution | **+14 %** (34.1 → 39.0–40.1 fps), the single biggest lever measured; exposed as the 动态分辨率 settings row (default off) |
| 720p-from-480p canvas | +14 % (34.1 → 39.0 fps), the game's own handheld mode |

## Why frames drop during play (measured while playing, 2026-11-25)

The long-frame log (整帧 > 80 ms) shows the drops are **first-visit texture streaming**, not steady-state cost:
one frame uploaded 383 textures / 280 MB (146 ms of upload, 582 ms frame with a 343 ms GPU burst), others
84-145 textures / 25-135 MB (100-400 ms frames) right after the save loads and when new areas enter view.
Rarer: one arena heap migration (~90 ms) and a one-off pipeline compile (~17 ms). Steady state is p50 ~24 ms
at 720p on the M2 — the drops are area loads, which the shader cache / pipeline archive already cover for
shaders; textures stream on demand by design.

## 1080p native on the M2 (2026-11-26): 60 FPS displayed, ~40-52 real

Measured at canvas 1920x1080 (the "1080p (native)" row), same scene/method on the rebased tree:

| Config | game fps | displayed FPS |
|---|---|---|
| 1080p native, fixed scale | 34.9 (p50 28.6 ms) | 34.9 |
| 1080p native + dynamic resolution | **51.5 (p50 19.3 ms)** | 51.5 |
| 1080p native + DRS + interpolation 30+30 | 30 (held) | **59.5-60.0** |
| 1080p native + DRS + interpolation **uncapped** (`interp-4`) | **~40 real** | **60.0 steady** (present interval p50/p95 16.67 ms, 373-399 of 600 presents interpolated, 0.2-0.7 % over one refresh) |

So at 1080p native the M2 shows a steady **60 FPS** with the uncapped interpolation while the game logic
runs at ~40 fps (DRS adapts the internal 3D scale, ~683x384-768x432 here). A real 60 fps *game* rate at
1080p native is not reachable on the M2 in this scene: the GPU wall needs <= 16.7 ms/frame and DRS already
sits at ~19.3 ms; 51.5 fps is the measured ceiling without interpolation.

## 60 FPS displayed on the M2: the applied recipe (2026-11-25)

With the settings files alone, measured on the rebased tree: present interval **p50 / p95 / max 16.67 ms,
~0 % over one refresh, ~300-400 of 600 presents interpolated — a steady 60.0 FPS on the 60 Hz display**.
- 720p native (`graphics-canvas.txt=1`) + `graphics-drs.txt=1` + `graphics-frame.txt=interp-1` (30+30): 60.0 displayed, game held at 30.
- 1080p native (`graphics-canvas.txt=3`) + `graphics-drs.txt=1` + `graphics-frame.txt=interp-4` (uncapped): **60.0 displayed, game ~40 fps real** — the current applied setting.
Without interpolation the same scene runs at 41 fps (720p native) / 35 fps (1080p native), 51 fps with DRS.

## Update after rebasing on 786ddc00 (PR #2324, 2026-11-25): pass splits on M1/M2

`origin/main` then landed "on M1 / M2 an Ocarina of Time render pass ends only where mesh and ordinary draws meet"
(PR #2324): render passes per frame in this scene went **159.7 -> 114.2** (-28 %) and the frame rate on the M2
at 720p native went **34.1 -> 41 fps** (p50 29.2 -> 24.2 ms). New numbers on the rebased tree, same scene/method:

| Config | game fps (p50) |
|---|---|
| 720p native | 40.9-41.2 (24.2 ms) |
| 720p native + dynamic resolution | **51.0 (19.5 ms)** |
| 480p canvas | 41.7-49.7 (20-24 ms) |
| 480p canvas + dynamic resolution | **50.8 (19.6 ms, p99 22.3 ms — the most stable)** |

So on the M2 the game now runs at ~41 fps at 720p native and ~51 fps with the dynamic-resolution row on —
"aux alentours de 60" on the game rate in the heavy village scene, with the display filled to a steady 60 Hz
by MetalFX interpolation 30+30 whenever the game rate is >= 30 (measured displayed 54-60 FPS with FI on;
note FI itself costs ~2-4 ms of GPU per generated frame, so it trades a little game rate for the steady 60 Hz).

## Reaching a steady 60 Hz on screen on the M2 (display rate, with frame interpolation)

The game runs its logic at ~30-40 fps in this scene on the M2 (GPU wall above); MetalFX frame interpolation
fills the display to a steady 60 Hz. Measured with metalperftrace (output FPS = drawables shown per second):

| Config (720p canvas unless said) | game fps | **displayed FPS** | on-GPU avg |
|---|---|---|---|
| 720p native, no interpolation | 34 | 34 | 27.9 ms |
| 720p native + interpolation 30+30 | ~30 | **55.5-57** | 17.5 ms |
| 720p native + dynamic resolution + interpolation 30+30 | ~30 (internal 683x384-1152x648 adapting) | **60.0 steady** (present interval p50 16.67 ms, ~50 % interpolated, 0.3 % over one refresh) | ~16 ms |
| 480p canvas (720p from 480p) + interpolation 30+30 | 29.9 | **60.0 steady** | 15.9 ms |

So on an M2 the practical "60 FPS" setup is: Resolution 720p native (or 720p from 480p), the new 动态分辨率 row on,
and 补帧 (MetalFX) 30+30 — the display then shows a steady 60 images/s. The interpolation costs GPU time per
generated frame (~2-4 ms), which is why the game rate itself drops a little; the DRS row is what keeps the game
rate (and latency) up while it adapts.

## Update 2026-11-25 (later the same day)

- The pending-write overlay scan now skips out-of-span queries (perf commit in this PR): the copy routine's
  scans went 15.6 -> 0.0 entries per call (~29k iterations/frame removed), applied entries identical;
  `probe-gpu-ubo-snapshot` passes (16,409 checks). Frame rate unchanged — the frame is GPU-bound, this removes CPU work.
- 480p canvas + the game's dynamic resolution together: 39.5-39.9 fps (DRS adds nothing on top of 480p there,
  the game is already at its lowest scale).

## Where the remaining work is

1. **Per-draw / per-pass cost** (~1,200 draws, ~160 passes/frame): more pass merging (only 8 layered passes merge
   today), fewer encoder splits, less attachment load/store traffic. The mesh-batch and write-domain work of
   2026-11-08 already landed; the next step is batching more of the indirect-argument preparation and looking at
   the ~31 compute passes (mesh + culling) — the offline meshbench showed splitting one thread's reads across
   SIMD groups is worth up to −42 % on the mesh stage, that is a translator (`tools/totk_xlate`) change and needs
   a new chain and seed.
2. **Shadow passes** (3 depth-only 1000×1000 passes/frame, ~3.3 ms): resolution-independent; only reducible by
   rendering them less often, which is the game's decision, not ours.
3. **CPU headroom** for when the GPU is not the wall (lighter scenes): the HLE scheduler still shows in samples
   (`runtime_gate` / `svc_thread_` / `block`); the `yield_backoff` / `yield_fast` flags of 2026-11-24 are off by
   default and stay off — on the M4 they saved ~2.5 % CPU but dropped frames at 100 fps.

Rule reminder for any change here: on the Mac the picture must not change — no resolution, effect or rate changes
by default; anything that lowers quality goes behind a switch that is off by default (the dynamic-resolution row
is exactly that: the game's own console behavior, opt-in).
