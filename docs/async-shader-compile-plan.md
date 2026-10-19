# Plan: stop in-game shader compiles from stalling frames

Status: plan, not implemented (2026-10-19). Mac only.

## What the last run shows

Source: `~/Library/Logs/MacNativeGames/totk.log` from the 2026-10-19 07:21 session (1.4.3, seed imported earlier, 0 files imported this launch). This log is replaced on the next launch, so copy it to `/Volumes/mac-APFS/MacNativeGames-artifacts/<date>/` if it needs to be kept.

- 61 timing windows of 300 frames each. 25 windows have a frame over 50 ms and 21 have one over 100 ms. The worst frames are 13,049 ms (frames to 14,100), 6,148 ms (to 14,400) and 3,012 ms (to 17,700).
- In every bad window, the time is in `NVN 提交+出图` (submit + present), for example 13,029 ms of the 13,049 ms frame. GPU time stays under about 24 ms. The stall is CPU-side, on the submit thread.
- More than 300 new shader variants were translated during play (`[shader] translated 300`), and the variant warm-up compiled all 9,372 libraries from source: `着色器库：预编译加载 0 个`. **None of the prebuilt `.metallib` files from #16 were used.**
- Startup waited 284 s for shaders. It then started with 0 render pipelines and 0 compute pipelines ready, and the 2,542 collected render pipelines finished warming in the background 312 s after launch.
- Pipeline prediction kicked 434 pipelines in total. The last kick is at log line 21,239 of 38,716, and none appear after that, although the worst hitches come later (see bug 2 below).

## Why the merged "async" work does not help

No PR made in-game compiles asynchronous. The related merges are:

- **#13 parallel warm-up** (`warm_precompile`) only fills the system Metal cache during the startup warm-up.
- **#16 precompiled shaders** (`step_shader_libs`) only covers the seed's MSL, and only when the build machine has the Metal toolchain. It loaded 0 libraries in the last run.
- **Pipeline prediction** (`pso_pend_kick` / `pso_pend_take` / `pso_predict_rest`, [nvn_mtl.m:5799-5859](../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m#L5799-L5859)) compiles later pipelines of the same submission **in parallel**. The submit thread still **waits** for each one (`pthread_cond_wait` in `pso_pend_take`), so a frame cannot finish until every new pipeline exists.

On the Mac, a draw that meets new work goes through up to three blocking steps, all on the submit thread:

1. **Translation.** `prog_get` / `prog_fit` call `totk_rt_variant` ([totk_shader_rt.m:147](../games/totk/third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m#L147)). On a miss, it makes a blocking pipe round-trip to the one translator service process while holding the global `g_rt_lock`.
2. **Metal library.** `shader_get` calls `shader_load`, which runs `newLibraryWithSource` synchronously.
3. **Pipeline.** `pso_get` runs `newRenderPipelineStateWithDescriptor` synchronously, or waits in `pso_pend_take`. Compute `dispatch()` runs `newComputePipelineStateWithFunction` synchronously.

## Bugs that make it worse

1. **Prediction translates on the submit thread.** `pso_predict_rest` calls `prog_get` for every *future* draw in the submission. Every untranslated program there is translated and compiled synchronously, one at a time, before the current draw continues. One miss can therefore pull a whole submission's worth of new shaders into one frame.
2. **The prediction table fills up permanently.** Slots in `g_pp[384]` are freed only when a draw takes them (`pso_pend_take`). The predictor deliberately guesses up to 4 depth/stencil attachment variants per draw, so guesses that are never used keep their slots forever. Once all 384 are in use, `pso_pend_kick` silently stops kicking, and every later miss is a one-by-one synchronous compile. The last run kicked 434 in total and logged no kick after line 21,239. This fits the table being full, but it is not proven, because the number of pipelines taken is not logged.
3. **Prebuilt libraries were not loaded** (0 of 9,372). Either the build had no Metal toolchain, or the MSL in the user cache no longer hashes to the seed MSL that was compiled (`shader_lib_prebuilt_path` uses SHA-256 of the cache file's bytes). The App was not at `/Volumes/mac-APFS/MacNativeGames-artifacts/apps` when this plan was written, so this was not checked.

## Quick fix

The idea: **never block a frame on a compile; skip the draw until its shader or pipeline is ready.** The other emulators' "asynchronous shaders" options work the same way. Most of the code already exists for iOS:

- `NvProg.missing` / `miss_sig` / `miss_has`
- `g_draw_skip[15]`
- `dispatch()` skips when `missing` is set

The work is to make the Mac take that path and fill the work in from the background.

### Step 0: cheap fixes, landed on their own first (≈30 lines)

These stand alone and should be measured on their own before step 1.

- **Free the prediction table.** At the start of each submission, and before kicking, move every `done` slot in `g_pp` into `g_pso` with `hm_put_id` on the submit thread (the only thread that touches `g_pso`), then free the slot. Never-used guesses become cached pipelines instead of leaked slots. Log kicked / taken / drained / full counts with the existing timing line.
- **Stop prediction from translating.** Give `prog_get` a "lookup only" mode for the predictor: when a stage needs translation, return without translating and skip that draw's prediction. In step 1 this becomes "enqueue the translation" instead.
- **Find out why prebuilt libraries did not load.** Check the build log's `3/4 shaders:` line and `<work>/logs/shader-libs.log`, whether `Contents/.../build/shader-libs/*.metallib` is in the App, and, for one key, whether the SHA-256 of the cache `.metal` matches the seed `.metal`. Fix whichever one is broken. This also shortens the 284 s startup wait.

### Step 1: non-blocking misses (≈250 lines in `nvn_mtl.m` + ≈60 in `totk_shader_rt.m`)

Behind `TOTK_ASYNC_SHADERS`: default `1`; `0` restores today's blocking behaviour for A/B runs.

1. **Pipelines.** In `pso_get`, on a cache miss for a normal pass:
   - build the descriptor on the submit thread, as now;
   - compile it on a background queue (the existing `pso_pend_kick`);
   - add `pso_pend_poll()`, which returns *ready* (move to `g_pso`), *pending*, or *absent* (kick it, then *pending*);
   - when the result is *pending*, skip the draw under a new skip reason.

   Return a distinct "pending" result so pending draws do not hit the `[pipeline-draw]` failure log or `g_draw_skip[9]`. Merged-pass and specialised-fragment pipelines (`g_pso_lmap`) stay synchronous in this step, because they are rare. If the table is full, compile synchronously as today.
2. **Shader variants.** Split `totk_rt_variant` into two calls:
   - **lookup**, which matches deps in the in-memory index and does no translation;
   - **request**, which enqueues a translation job.

   On a miss, `prog_get` / `prog_fit` mark the stage missing, exactly as on iOS, and enqueue one job per (shader, answers hash). The job:
   - translates through the service;
   - calls `shader_load` off-thread. The warm-up thread already does this, so `shader_load` is known to run off the submit thread;
   - posts `{variant, NvShader}` to a completion list.

   Requirements:
   - **Copy the guest bytes on the submit thread before enqueueing**: the control section (0x880 bytes), the code (`c1t` bytes), `rt_answers(st)` and `rt_local`. The program's guest memory may be freed or reused before the job runs.
   - **Do not hold `g_rt_lock` across the pipe round-trip.** Use a separate lock for the service's stdin/stdout, so that a lookup on the submit thread never waits behind a translation.
3. **Re-resolving programs.** At the start of each submission, drain the completion list:
   - insert each shader into `g_shcache`, and record each variant in the program-side index;
   - bump a generation counter.

   `prog_get`'s cached `NvProg` with `missing != 0`, and `prog_fit`'s `miss_sig` path, retry when the generation has changed. Today they treat a miss as permanent, because on iOS nothing fills it in during the session. The first-result cache in `prog_get` (`lp` / `lser` / `lpr`) must not return a missing program after the generation moves.
4. **What still blocks.**
   - **Compute dispatches.** Compute translation misses and compute pipeline compiles stay synchronous. Compute writes memory: occlusion culling, and the NPC-culling fix in `drv_scan`'s note. Skipping a dispatch can leave stale data for many frames. The last run had only 39 compute pipelines.
   - **A per-frame stall budget**, `TOTK_SHADER_STALL_MS`, default `4`. A pending job may be waited on for up to the frame's remaining budget (`pthread_cond_timedwait`) before the draw is skipped. A job that finishes quickly then costs nothing visible, and a long one costs at most the budget once per frame. `0` means always skip, and a large value approximates today's behaviour.
5. **Logging.** Add to the existing 300-frame timing line:
   - draws skipped while waiting for a shader, and while waiting for a pipeline;
   - jobs queued, done and failed;
   - the longest job;
   - stall-budget time used.

   Keep `[survey-shader]` timings. A failed background translation must be reported as `die()` is today, not lost on a background thread. Post the failure, and have the submit thread `MDIE` on the next drain, unless an explicit policy says otherwise.

### Risks to check during testing

- **Missing geometry for a few frames** while a job runs. This is the intended trade-off. It is visible as objects or effects popping in.
- **One-shot render-to-texture draws.** A draw rendered only once (a baked texture, a capture, a cube face rendered on demand) and skipped stays wrong until the game redraws it. Look for these in the first-run comparison. If one shows up, run that pass synchronously through the stall budget, or keep skip-free draws for passes whose target is not the frame's colour chain.
- **Draws that store to memory**, for example the deferred-lighting tile feedback word that blacked out the title mural. A skipped draw leaves the previous value for one frame. Check that the title mural and the lighting are correct after the job lands.
- **Occlusion and counter queries around a skipped draw** report 0 samples, so the game may cull that object for a frame or more. Check NPC and object visibility on a cold cache.
- **Thread safety of `shader_load` off-thread:**
  - `c1_store` (shared c1 tables);
  - `drv_scan`'s `MLOG`;
  - the non-atomic `g_rt_prebuilt` / `g_rt_compiled` counters.

  The warm-up thread already runs it concurrently with the game, so either these are safe or the race exists today. Audit them either way.

## Test

Follow the method and minimum record in [performance-followups.md](performance-followups.md). Use a separate profile and an empty translation cache in the user data directory's `tests/` subdirectory, not the real profile.

1. **Cold-cache A/B.** Use the same build and route (start of game → first sky island, plus one fast-travel). Run 8 × 30 s windows in the order A/B/B/A/B/A/A/B, with `TOTK_ASYNC_SHADERS=0` against `1`. Delete the translation cache between runs. Report:
   - frame-time p50, p95, p99 and maximum for each window;
   - the number of frames over 50 ms and over 100 ms;
   - the new skip counters.

   Success: no frame over 100 ms caused by compiles, with p99 within 2× of a warm-cache run.
2. **Warm-cache regression.** Repeat on a warm cache. Skip counters should be about 0 and p50/p95 unchanged within noise.
3. **Visual check.** Compare screenshots of the title, the first lit interior and an NPC area against `TOTK_ASYNC_SHADERS=0` after the jobs have drained.
4. **Step 0 alone.** Measure the step 0 fixes (table drain, prediction without translation, prebuilt libraries) on their own before step 1, so each change gets its own result rather than one combined "FPS fix".

## Later steps

Each step below is a separate PR with its own measurement, done in this order. Steps 2 and 3 matter most once step 1 has landed: they shorten how long draws are skipped and how many are skipped at all. Step 4 removes repeat work across launches, and step 5 closes the last synchronous path.

### Step 2: several translator processes (≈120 lines, `totk_shader_rt.m`)

**Why.** `service_start` ([totk_shader_rt.m:78](../games/totk/third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m#L78)) spawns one `totk_xlate --service` process with one stdin/stdout pair, so translations run one at a time. Each job also starts a `spirv-cross` child process ([Service.cs:264](../games/totk/tools/totk_xlate/Service.cs#L264)). After step 1 this no longer stalls frames, but a burst of new variants (a new area, a fast-travel) still drains serially, and every draw that needs them stays skipped until the last one finishes.

**Why it is safe to run several.** Each job writes only files named after its own variant (`<variant>.code`, `_b.metal`, `.res.json`). The reflection `_b.json` is written last, through `.tmp` and an atomic rename ([Service.cs:350-352](../games/totk/tools/totk_xlate/Service.cs#L350-L352)), and the runtime treats a variant as present only once that file exists. Two processes are unsafe only if they are given **the same variant** at the same time, and the job queue prevents that.

**Changes:**
1. **A pool of services.** Replace `g_svc_in` / `g_svc_out` with an array of up to `N` slots `{pid, in, out, busy}`. `N = TOTK_SHADER_SERVICES`, defaulting to half the performance cores, at most 4. Use `sysctl hw.perflevel0.physicalcpu` to count them, and leave cores for the game and Metal's compiler. A slot's process starts the first time the slot is used, so an idle session still starts only one.
2. **A job queue.**
   - Use one concurrent `dispatch_queue` and a counting semaphore of `N`.
   - A job takes a free slot, writes its request line, reads the reply, and frees the slot.
   - Keep an in-flight set keyed by variant name (`<shader>-<answers hash>`), so that a second request for the same variant joins the first job instead of starting another.
3. **Locks.**
   - Keep `g_rt_lock` only for `g_variants`, `g_msl_checked` and the `.variants` index write. It is held for microseconds, never across I/O.
   - Each slot gets its own lock for its pipe.
   - Each `.variants` index file is written while holding `g_rt_lock`, as now. `g_translated` and the other counters become `_Atomic`.
4. **Crashes.** If a service exits (`fgets` returns NULL), mark the slot dead, respawn it once, and retry the job once. A second failure is fatal as it is today: post the error, and the submit thread `MDIE`s on the next drain, as described in step 1.
5. **Shut down cleanly.** Close every service's stdin when the game exits. Each service exits on EOF.

**Measure.** `[survey-shader]` translation times, queue length, and how long the longest skip lasts, during the cold-cache fast-travel from step 1's test. Compare `TOTK_SHADER_SERVICES=1` with the default. Success: the time to drain a burst scales with `N` while frame-time p95 does not get worse. If p95 does get worse, the translators are competing with the game for CPU, so lower the default.

### Step 3: render pipelines ready before play (≈150 lines, `nvn_mtl.m` + `hle_nvn.c`)

**Why.** The last run started with **0 render pipelines** ready.
- The first-frame gate in `warm_start` ([nvn_mtl.m:6198](../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m#L6198)) waits only for `g_warm_priority_done`, which is set after the **first** list: the prepared seed's `pso_manifest.tsv`, made of `S` (shader) rows.
- The collected `R`/`C` pipeline rows come next, in the repo `pso_manifest.tsv`, then `pso_user.tsv`, then `pso_collected.tsv`.
- `warm_worker` ([nvn_mtl.m:6008](../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m#L6008)) builds those pipelines **one at a time** on a serial queue. `warm_precompile` makes the libraries parallel, not the pipelines.
- Loading a save waits at most 15 s for the warm-up (`mtl_warm_wait(15000)` in `hle_nvn.c:2576`), then enters play while the rest continues.

In the last run, the warm-up finished 312 s after launch, with 2,542 render pipelines.

**Changes:**
1. **Build pipelines in parallel.**
   - In `warm_worker`, collect the `R` and `C` rows whose shaders are loaded into a batch, about 64 at a time.
   - Build the batch on a concurrent queue limited to `perflevel0` cores, or through `newRenderPipelineStateWithDescriptor:completionHandler:`.
   - Keep `warm_enqueue` order and progress counting the same as now.

   This is the same idea as `warm_precompile`, but for pipelines, and pipeline builds are mostly system-cache hits once their functions are compiled.
2. **Pipelines for this profile first.** Change the list order to:
   1. the seed `S` rows;
   2. **this profile's `pso_user.tsv`, newest rows first**, because those pipelines are most likely needed in the save being loaded;
   3. the repo manifest;
   4. `pso_collected.tsv`.

   Set a second flag, `g_warm_profile_done`, after list 2.
3. **The gate when loading a save.** Make `mtl_warm_wait` wait for `g_warm_profile_done` rather than the whole warm-up, still with a timeout. Raise the default to 60 s and make it configurable with `TOTK_WARM_WAIT_MS`. Show progress on the existing loading overlay (`runtime_warm_progress`).

   With step 1 in place, a timeout is no longer catastrophic. Whatever is not ready gets compiled asynchronously when first drawn.
4. **Lower the warm-up's priority once in the world.** After `mtl_note_world()`, lower the rest of the warm-up queue to `QOS_CLASS_UTILITY` (or re-enqueue it at that priority), so it does not compete with step 1's on-demand jobs, which stay `USER_INITIATED`. iOS's `warm_light` already handles world entry this way.
5. **Hand warmed pipelines over before a draw compiles them again.** While the warm-up is running, step 1's `pso_pend_poll` should look in the warm-up's claimed items (`warm_claim`, [nvn_mtl.m:9471](../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m#L9471)) before kicking its own compile, so the same pipeline is not compiled twice.

**Measure.** Time to the first frame, the time at the save-load gate, render pipelines ready at the gate, and step 1's skip counters in the first 60 s of play, on a profile with an existing `pso_user.tsv`. Success: no draws skipped while waiting for a pipeline in the first minute, on a profile that has played the same area before.

### Step 4: keep pipelines across launches (≈200 lines, `nvn_mtl.m`)

**Why.** Today, pipelines persist only through the system Metal cache. That cache starts over whenever the App is moved or rebuilt ([first-launch-shaders.md](first-launch-shaders.md) measured a full 4.5-minute warm-up again), which happens on every build in this workflow. A binary archive is a file we own: it survives rebuilds and moves, and loading a pipeline from it skips the back-end compile.

**API choice.** The renderer uses Metal 4 command queues (`MTL4CommandQueue`), but it creates pipelines through the classic `MTLDevice` API (`MTLRenderPipelineDescriptor`). So use **`MTLBinaryArchive`** (in the SDK, `MTLBinaryArchive.h`: `addRenderPipelineFunctionsWithDescriptor:`, `addComputePipelineFunctionsWithDescriptor:`, `serializeToURL:`). The Metal 4 route (`MTL4Compiler` + `MTL4PipelineDataSetSerializer` + `MTL4Archive`, macOS 26+) would mean moving `pso_desc` ([nvn_mtl.m:5696](../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m#L5696)) to `MTL4RenderPipelineDescriptor`. That is a separate migration, worth doing only if the classic archive turns out to be insufficient.

**Changes:**
1. **The file.**
   - Path: `<totk_rt_dir>/pipelines-<DMSH_CHAIN>-<gpu registryID name>-<OS build>.mtlarchive`, in the external shader cache, never in the App or the repo.
   - The OS build and GPU in the name keep stale archives from being used. Metal treats a mismatched entry as a miss anyway, but separate files keep the hit rate easy to read.
   - Delete archive files for other OS builds when the game starts.
2. **Load at startup.** Create the archive with `newBinaryArchiveWithDescriptor:` (with `url` set) before `warm_start`. If loading fails (corrupt file), log it, delete the file, and start with an empty archive.
3. **Use it.**
   - Set `descriptor.binaryArchives = @[archive]` in `pso_desc` and for compute pipelines.
   - Build the compute pipelines from an `MTLComputePipelineDescriptor`, not `newComputePipelineStateWithFunction:`, so the archive applies.
   - Measure the hit rate in a diagnostic mode, `TOTK_ARCHIVE_PROBE=1`, using `MTLPipelineOptionFailOnBinaryArchiveMiss` on a copy of the descriptor. Never use that option in normal runs.
4. **Record.**
   - After every successful compile that was not from the archive (the step 1 jobs, the warm-up and the predictor), call `add…PipelineFunctionsWithDescriptor:` on one serial `archive` queue. Do not assume the archive is thread-safe for adds.
   - Count the additions, and serialise to `<path>.tmp` followed by an atomic rename in three cases: every 200 additions, at the save-load gate, and on clean exit through the existing window-close path ([frontend.m:169](../games/totk/runtime/frontend.m#L169)).
   - Never serialise on the submit thread.
5. **Limits.** The archive grows with every pipeline. Log its size. If it passes `TOTK_ARCHIVE_MAX_MB` (default 512), start a new archive from the warm-up lists at the next launch instead of growing it.
6. **Switch.** `TOTK_PIPELINE_ARCHIVE=0` turns all of this off, for A/B runs.

**Measure.** With the same profile, compare second launches after a **rebuild** of the App, with the archive and without (`TOTK_PIPELINE_ARCHIVE=0`). Record:
- the warm-up duration;
- `g_warm_pipeline_ns` per pipeline;
- `[survey-shader]` pipeline times;
- step 1's skip counters in the first minute.

Also record the archive's size and its hit rate (`TOTK_ARCHIVE_PROBE=1`). Success: after a rebuild, warming the pipelines takes seconds, not minutes.

Not covered: libraries built from MSL source (`newLibraryWithSource`) still go through the front-end compiler on each launch, unless step 0's prebuilt `.metallib` files or the system cache cover them. An archive stores pipeline back-end binaries, not the MSL front-end. For variants translated at runtime, a later option is to write their `.metallib` with `MTLDynamicLibrary`, or to run the offline `metal` compiler on them in the background when the toolchain is present.

### Step 5: async merged-pass and specialised-fragment pipelines (≈100 lines, `nvn_mtl.m`)

**Why.** Step 1 keeps `g_pso_lmap` pipelines, which are merged readable-tile passes, synchronous. Two things in them can compile on the submit thread:
- `bf_fn` ([nvn_mtl.m:8056](../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m#L8056)) runs `newFunctionWithName:constantValues:`, a real compile, while holding `g_fz_fn_lock`;
- `pso_get` then builds the pipeline with that function.

**A safe fallback already exists.** The merge planner `fz_p2_draw` ([nvn_mtl.m:8163](../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m#L8163)) returns `0` ("cannot prove") whenever it cannot use the specialised version. The pass is then drawn **unmerged**, which is correct but slower. So, unlike step 1, nothing is skipped: the frame falls back to the unmerged path while the compile runs.

**Changes:**
1. **Split `bf_fn`.**
   - `bf_fn_lookup` returns the cached function, or nil.
   - `bf_fn_request` enqueues the `newFunctionWithName:constantValues:` compile on the step 1 queue, keyed by the existing `cacheKey`, and puts the result into `g_bf_fn_cache` under `g_fz_fn_lock` when it is done.
2. **`fz_p2_draw`.** When the lookup misses, request the function and return `0`. The pass is not merged this time, and a later pass with the same shape merges once the function exists.
3. **`pso_get` with `g_pso_lmap`.** When the function exists but the merged pipeline does not, use the same `pso_pend_poll` as step 1. The planner has already committed to merging by then, so a pending result here cannot fall back. Either:
   - (a) wait within step 1's stall budget, then skip; or
   - (b) better: have the planner also check the merged pipeline (`hm_get_ptr(&g_pso, …)` with the planned key), and refuse to merge until both the function and the pipeline are ready.

   Prefer (b). It needs the pipeline key to be computable in the planner, which `pso_predict_rest` already shows how to do.
4. **The warm-up** already rebuilds `fz` rows from the manifests ([nvn_mtl.m:6036-6041](../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m#L6036-L6041)). Keep that, so collected merged pipelines are ready before play and the fallback is rare.

**Measure.** Count passes merged and passes refused because the function or pipeline was pending, and take frame-time p95 / p99 over the first minutes on a cold cache. Compare against step 1 alone, then against a warm cache to confirm the merge rate recovers. Success: no frames over 50 ms from merged-pass compiles, and the same merge rate as today on a warm cache.

### Order and what each step depends on

| Step | Depends on | Removes |
|---|---|---|
| 0 | none | prediction table leak, predictor translating, prebuilt-library miss |
| 1 | 0 | frame stalls from translation, library and pipeline compiles (normal passes) |
| 2 | 1 | the serial translation backlog behind skipped draws |
| 3 | 1 (for the timeout to be safe) | pipelines missing at the start of play |
| 4 | none, but measure after 3 | full pipeline rebuilds after an App rebuild or move |
| 5 | 1 | the last synchronous compiles (merged passes) |
