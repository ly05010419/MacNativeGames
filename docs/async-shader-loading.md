# Load in-game shaders and pipelines in the background

## Problem

When play reaches a shader variant or render pipeline it has not used before, all of the work happens on the submit
thread, inside the frame:

1. **Translation.** `totk_rt_variant` asks the translator service for the MSL.
2. **Metal library.** `shader_load` runs `newLibraryWithSource`. For a variant translated this session, this is a cold
   front-end compile of about 100–900 ms.
3. **Pipeline.** `pso_get` runs `newRenderPipelineStateWithDescriptor`, or waits for a predicted one in `pso_pend_take`.

The frame cannot be presented until every new library and pipeline exists. In the cold-cache runs below, frames over
100 ms spent 79% of their time in step 2 and 13% in step 3. The worst frames lasted 10–17 s.

## Change

`TOTK_ASYNC_SHADERS` (default `1`) moves steps 2 and 3 off the submit thread for vertex and fragment work. A draw whose
library or pipeline is not ready yet is skipped and retried on later frames.

- **Libraries.** `prog_get` / `prog_fit` call `shader_get_async`, which:
  - queues `shader_load` on a background queue, once per variant;
  - waits up to `TOTK_SHADER_STALL_MS` (default 4 ms), so a variant already in the system Metal cache is not skipped;
  - otherwise marks the stage pending (`NvProg.pend*`). Its draws are counted as `shader-loading` (`g_draw_skip[17]`).

  `mtl_sub_begin` publishes finished loads into `g_shcache` (`shload_drain`). The program picks them up the next time
  `prog_get` returns it (`prog_retry`). A retried stage uses the binding signature it was translated for.
- **Pipelines.** `pso_get` kicks a normal-pass pipeline miss into the existing pending table as a *demand* slot, then
  polls it with the same budget (`pso_pend_poll`).
  - If the pipeline is not ready, the draw is counted as `pipeline-pending` (`g_draw_skip[16]`). This is not reported
    as a failed pipeline.
  - Demand slots are not reclaimed as stale guesses; the draw takes them when it comes back.
  - If the table is full, the pipeline compiles synchronously, as before.
- **Unchanged.** Translation (step 1) stays synchronous, so no guest memory crosses threads. Compute shaders and
  pipelines stay synchronous, because skipping a dispatch can leave stale data for many frames. Merged-pass and
  specialised-fragment pipelines (`g_pso_lmap`) also stay synchronous, as does iOS.
- **Failures.** A library that fails to load is still fatal (`MDIE`) when a program needs it, as with `shader_get`.
- **Prediction.** It skips programs that have a stage still loading.
- **Logging.**
  - `[长帧]` now splits creation time into library and pipeline time, and adds the translations made in the frame and
    their time (`totk_rt_xlate_stats`). The part `scripts/summarize_mac_soak.py` parses is unchanged.
  - Every 300 frames, `[nvn] 后台载着色器库` reports background loads queued, finished and failed, with their average
    and longest time.

`TOTK_ASYNC_SHADERS=0` restores the previous behaviour completely. `TOTK_SHADER_STALL_MS` sets the wait budget (`0`
never waits).

## Two fixes this needed

Testing async mode with an empty translation cache turned up a GPU address fault on the first presented frame, in
4 of 14 boots: `上屏命令缓冲错误 … kIOGPUCommandBufferCallbackErrorPageFault`. Sync mode had none in 12 boots.

- **The present source was not resident.** At frame 1 the game's first draws need freshly translated shaders, so in
  async mode they are all skipped. No pass then renders into the presented texture, and `present_now` creates and
  uploads it in `tex_mtl`, adding it with `res_add`. The residency set was committed only on the optional flash-guard
  path, so the present command buffer could read a non-resident texture.
  - Fix: `present_now` calls `res_commit()` after taking its source. This does nothing when nothing is new.
  - Result: 0 faults in 12 async boots, against 4 in 14 before.
- **`totk_precise_opts()` race.** It published its `MTLCompileOptions` before setting `mathMode`. Two threads
  compiling at once (the warm-up and the background loads, both at startup) could compile one shader with the
  default fast math. It is now set up with `dispatch_once`. This race was not the cause of the fault above.

## Border colour emulation and the per-frame stall cap (2026-10-20)

Upstream's sampler border colour emulation (`fs_fn_border`, `PSO_VBORDER`) brought back long frames in async mode.
A 1.4.3 session had frames of 0.5–2.3 s that were all "pipeline" time:

- **Patched functions compiled on the submit thread.** A border variant is the shader's MSL rewritten and compiled
  from source (`fs_fn_border` in `pso_get`, `vs_fn_for` in `pso_desc`), a cold front-end compile like a library's.
  Neither went through the background path.
- **Waits added up.** Draws of a shader that has met a border sampler look up their pipeline on every draw. With
  several pipelines pending, each draw that switched to another one waited the full `TOTK_SHADER_STALL_MS` again:
  3,180 waits and 2.2 s in one frame.

Changes:

- `fn_patch_async` compiles patched vertex functions (unwritten varyings, layer index, border colours) and border
  fragment functions on a background queue. `pso_get` waits at most the stall budget for them, and otherwise skips the
  draw as `pipeline-pending`. The functions are stored in the shader's `vpatch` / `bpatch` dictionary, under its lock;
  compiles run outside the lock. Pipeline guesses (`pso_pend_kick`) only queue such a compile and never build one on
  the submit thread. `TOTK_ASYNC_SHADERS=0`, merged passes and the warm-up keep compiling them in place.
- Every wait of the submit thread (libraries, pipelines, patched functions) also counts against a per-frame cap,
  `TOTK_SHADER_STALL_FRAME_MS` (default 8).
- A draw still waiting for its pipeline no longer counts as a created pipeline in `[长帧]`.

Measured with the same cold-cache method as below (empty translation cache, `MTL_SHADER_CACHE_SIZE=0`, the newest
save, the same scripted route), two runs per build, on a profile whose warm-up list (11,592 rows) kept the CPU
saturated, so whole-frame numbers were noisy. Submit-thread stalls on the route:

| | Before (upstream merge) | After | Before border emulation (`cea7608`) |
|---|---:|---:|---:|
| Stalls of 100 ms or more | 14, 16 | 4, 3 | 4, 12 |
| Time in them | 7.6 s, 7.9 s | 2.2 s, 1.4 s | 0.9 s, 2.9 s |
| Of it, pipeline creation | 3.8 s, 3.1 s | 0.7 s, 0.2 s | 0.7 s, 1.1 s |
| Longest stall | 1,480 ms, 1,677 ms | 1,259 ms, 661 ms | 352 ms, 702 ms |

Runs, logs and frame times: `~/Library/Application Support/MacNativeGames/TOTK/tests/stutter-ab-2026-10-20`.

## Trade-off

An object, effect or pass whose library or pipeline is still compiling is missing for those frames, usually a few
frames per new variant. In the runs below this was not noticeable in the end screenshots, but:

- a pass drawn only once while its work is pending (a capture or a baked texture) stays wrong until the game draws it
  again;
- occlusion queries around a skipped draw report 0 samples, so the game may cull that object for a frame.

## Measurement

**Setup.** Apple M4 (10 cores), macOS 27, TOTK 1.4.3. Both modes ran the same build, differing only in
`TOTK_ASYNC_SHADERS`. The 8-run comparison used a local build with the same async code, plus its own version of the
prediction fixes and the other open PRs #13 (switched off with `TOTK_WARM_PARALLEL=0`), #17, #18 and #19. This branch
(on #29), with the two fixes above, was then checked with two more pairs (below). Runs went in the order sync/async/async/sync/async/sync/sync/async. Each run used:

- a fresh copy of the same profile, with the newest save autoloaded;
- an **empty translation cache**;
- the **system Metal cache bypassed** (`MTL_SHADER_CACHE_SIZE=0`), so every library and pipeline compiled cold, as on a
  player's first visit to an area;
- `TOTK_WARM_PARALLEL=0`, the warm-up behaviour on `main`.

After the save finished loading (gameplay frames with more than 1,000 draws for 6 s) and 10 s to settle, a scripted
route ran for 69 s of wall time: camera turns, two sprints in different directions, then a look around. Frame times
come from `DM_NVN_TSV`. The breakdown comes from the `[长帧]` lines.

**Results.** Means over 4 runs per mode, with the range in parentheses:

| | Sync (`TOTK_ASYNC_SHADERS=0`) | Async (this PR) |
|---|---:|---:|
| Frames presented on the route | 1,798 (1,532–2,228) | 4,100 (4,004–4,236) |
| Average FPS on the route | 26.6 (24.4–32.2) | **59.3** (58.0–61.3) |
| p50 frame | 13.9 ms | 13.9 ms |
| p95 frame | 25.2 ms | 21.0 ms |
| p99 frame | 579 ms (460–683) | **71 ms** (53–79) |
| Worst frame | 13,067 ms (10,592–17,270) | **1,697 ms** (690–2,197) |
| Frames > 100 ms | 41.0 | 30.8 |
| Frames > 250 ms | 33.2 | **9.0** |
| Frames > 500 ms | 22.2 | **3.5** |
| Time lost to frames > 50 ms | 42.0 s | **10.1 s** |
| Save load to play | 60.0 s (59.0–60.5) | **22.5 s** (21.7–23.8) |

Where the time in frames over 100 ms went, per run:

| | Library creation | Pipeline creation | Translation |
|---|---:|---:|---:|
| Sync | 33.2 s | 5.3 s | 3.0 s |
| Async | 2.1 s | 1.1 s | 5.5 s |

This branch, same method, one pair before the two fixes and one pair with them:

| | Sync | Async | Sync (final) | Async (final) |
|---|---:|---:|---:|---:|
| Average FPS on the route | 31.4 | 60.5 | 39.1 | 59.3 |
| p99 frame | 166 ms | 65 ms | 429 ms | 81 ms |
| Worst frame | 16,281 ms | 1,138 ms | 2,677 ms | 892 ms |
| Frames > 500 ms | 15 | 5 | 21 | 3 |
| Time lost to frames > 50 ms | 38.0 s | 9.3 s | 27.5 s | 9.9 s |
| Save load to play | 59.6 s | 23.7 s | 62.2 s | 23.6 s |

**Notes.**

- **Async covered more ground.** A stalled frame also stops game time, so the async runs travelled further and met
  more new shaders: 280–347 background library loads per run. Translation time is higher in async for that reason.
- **The remaining long frames are mostly translation.** It is still synchronous. Making it asynchronous (copying the
  guest shader bytes first) is the next step.
- **Skipped draws, per async run:** 1,000–1,350 pipeline-pending and 1,800–3,000 shader-loading, out of about 4,000
  frames. No background load failed.
- **The 50–100 ms band is slightly larger in async** (58.8 frames over 50 ms against 50.8). That is mostly translation,
  and async presents 2.3 times as many frames over the same wall time.
