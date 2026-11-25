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
