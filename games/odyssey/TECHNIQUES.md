# Techniques reference for the Odyssey port

This is a working reference of techniques from upstream MacNativeGames, other recompilation projects, emulators, and Apple's Metal documentation, with what has been applied here. Project status stays in [`README.md`](README.md); this page does not claim results that the README does not.

Collected 22 October 2026. "Applied" means the code is in this tree and compiles. "Measured" means a bounded Odyssey run showed the effect.

## Applied from upstream MacNativeGames (origin/main `30a1b93`, our checkout is `43f53b4`)

Upstream is 359 commits ahead. Odyssey's renderer is a copy of the Bayonetta 3 one, and Bayonetta 3 got none of the newer work. The TotK copy did. These self-contained commits were backported as patches onto `third_party/d2r_gfx` and `runtime/threads.c`:

| Upstream | What it does | Upstream measurement | Odyssey state |
| --- | --- | --- | --- |
| `646ce29` (#186) shader prefetch | When the game calls `nvnProgramSetShaders`, usually while it loads, already-translated variants are compiled into Metal libraries on up to 4 background threads. The first draw finds the library ready. | TotK 1.1.2: stall frames 102 → 2, longest 4.3 s → 0.5 s | Applied, **opt-in** (`TOTK_SHADER_PREFETCH=1`). The first version hashed ~26,600 boot-time programs on the game thread. Odyssey now hashes after the first frame on a utility queue with `TOTK_PREFETCH_SLOTS` (default 2). A/B below. |
| `b5cbbca` (#166) prediction rescans | When a pipeline-prediction scan stops at its budget, the next miss scans again (up to 32 times), so a burst of new pipelines compiles in parallel. | TotK 1.2.1 world entry: pipeline time in long frames 35.8 s → ~1–7 s | Applied. `TOTK_PSO_RESCANS` (0 = old behaviour). |
| `2fa27d7` smaller GPU chunks | Mid-frame commits after 30 draws, then every 40, so the GPU starts sooner. | ~0.1–0.2 ms per frame | Applied. |
| `373725b` pool lookup | Per-thread cache of the last 8 memory pools, so the replay skips the pool mutex. | TotK shrine submit 5.86 → 5.56 ms | Applied. `TOTK_POOL_TLS=0` off; `TOTK_POOL_TLS_CHECK=1` verifies. |
| `f08266c` replay costs | Texture-slot generation counter, last pool hit first, cheap passdump check. | ~0.2–0.3 ms per frame | Applied. `TOTK_TEX_GEN_CHECK=1` verifies. |
| `dab728a` kernel sync | `SignalToAddress` wake-all writes `value - 2` as mesosphère does. A SystemEvent binding ends when the game closes the handle, and a re-created event gets a new binding. Waiters are only woken when one of their objects is signalled. | Correctness: BotW hung on a stale handle | Applied. `tests/system_event_bind/probe.c` passes all three parts against the new `threads.c`. The old `threads.c` fails it (negative control). License in `third_party/licenses/nxnative.txt`. |

Added for Odyssey: `MTLDevice.shouldMaximizeConcurrentCompilation = YES` (macOS 13.3+), so Metal's compiler service runs the warm-up, prefetch and prediction compiles side by side. It logs `maximumConcurrentCompilationTaskCount`; `DM_MAX_CONCURRENT_COMPILE=0` turns it off. Upstream does not set this flag. Blender and MoltenVK (`MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION`) do.

Rejected hunks resolved by hand: the `b5cbbca` call site (Odyssey has no pipeline archive, so it keeps `newRenderPipelineStateWithDescriptor:`), and the `646ce29` hook in `hle_nvn.c`, placed after Odyssey's `control_intern`. Patches and pre-change backups are in the session scratchpad only; the authoritative diff is upstream git (`git -C MacNativeGames show <hash>`).

### Odyssey A/B (22 October)

Four bounded 90 s runs resumed a copy of `Odyssey_build/profiles/cap-kingdom-start` with `TOTK_SHADER_ASYNC=1`, in the order off1, on1, off2, on2. Logs are `Odyssey_build/boot-ab-prefetch-*.log`; the harness is `Odyssey_build/capture/ab-prefetch/run_ab.py`.

| | off1 | on1 | off2 | on2 |
| --- | --- | --- | --- | --- |
| Startup critical-pipeline wait | 38.9 s | 0.23 s | 18.1 s | 0.28 s |
| Long frames after 45 s | 26 | 40 | 10 | 4 |
| Worst frame after 45 s | 644 ms | 174 ms | 104 ms | 87 ms |
| Compile inside long frames | 426 ms | 0 | 0 | 0 |
| Prefetch background compiles | — | 457 (6.5 s) | — | 347 (1.2 s) |

Each run warms the collected pipeline lists and Metal's system cache for the next run, so later runs are better whatever the setting. Prefetch-on was best in both pairs, but one run per setting cannot separate it from warming. The large swing in startup wait between the off runs is Metal-cache state (see upstream "Metal 的缓存": one writer at a time, eviction, path-length keying) and is not yet explained here. Next: 3+ alternating pairs after a fixed warm-up, or a cold-cache test with randomised MSL comments as upstream does.

## Pipeline binary archive — ported 22 October

**Since 21 November** Odyssey and Animal Crossing use Tears of the Kingdom's archive itself: the headers are shared (`shared/third_party/d2r_gfx/gfx/nvn/totk_pipeline_archive*.h`, linked into each game; `totk_pipeline_archive_port.h` holds the ports' `pso_new` / `cs_new`, `totk_pipeline_archive_port_fill.h` the `pipeline-archive` packaging mode). Files hold batches of pipelines with their `.keys`; the warm-up's thread writes what the archive lacked, nothing waits for an idle moment; the iPhone / iPad build compiles the device's pipelines on the Mac (`games/totk/ios/tools/device_pipelines.py make`, as Zelda's build), and with a valid archive the warm-up loads no shader library for itself. The notes below describe the first port (one file per pipeline, idle writes) and are kept for its measurements. `tests/test_ios_ports_unified.py PipelineArchive` checks the shared form.

From upstream `27baca0`, `6446e09`, `ea6c5a7`, `1eff989` and `610d735`, Mac path only. `nvn/totk_pipeline_archive.h` is the upstream header unchanged.

- **Where it is called.** `pso_new` and `cs_new` in `nvn_mtl.m` wrap every game pipeline creation: draw, prediction kick, warm-up, light warm-up and compute dispatch. Built-in helper pipelines are not wrapped.
- **How it works.**
  - There is one `MTLBinaryArchive` file per pipeline, under `build/shader-cache/rt-<chain>/pipelines/`.
  - A pipeline is first looked up with `MTLPipelineOptionFailOnBinaryArchiveMiss`. On a miss it is compiled as usual and queued for writing.
  - Writes run on a background queue when the game is idle.
  - Metal's `com.apple.gpuarchiver/gpuarchiver-*` work folders are removed after each write.
  - The archive is rebuilt when the OS, GPU or translation chain changes (`identity.json`).
- **Odyssey change.** `par_game_idle` does not require the TotK "in world" gate (>2000 draws per frame, unverified for Odyssey). Idle means warm-up finished and no compile for 3 s.
- **Off switch.** `TOTK_PIPELINE_ARCHIVE=0`. Upstream's iOS fill mode and its warm-list skipping were not ported.

Measured on 22 October: five bounded 90 s runs, each resuming a copy of `profiles/cap-kingdom-start`. All exited 0. Logs are `Odyssey_build/boot-ab-archive-arch-*.log`.

| Run | Archive | Startup warm-up | Note |
| --- | --- | --- | --- |
| fill | on, empty | did not finish in 90 s | no frame: 401 shader libraries compiled from source (cold Metal cache after the rebuild) |
| off1 | off | did not finish in 90 s | no frame: 601 libraries compiled from source |
| on1 | on, empty | 17.7 s | archive written during play (151 files) |
| off2 | off | 4.4 s | |
| on2 | on, 149 files | 1.6 s | archive grew to 430 files; one leftover gpuarchiver folder |

In-game long frames after 45 s were similar in on1, off2 and on2: 200–321 frames over budget, 5–7 over 100 ms. There were no in-game pipeline compiles on this route, so the archive's in-play benefit is untested here.

**The dominant cold-start cost is compiling shader libraries from source, not pipelines.** Fixing it needs precompiled `.metallib` files (`scripts/precompile_cached_shaders.py`, upstream `step_shader_libs`). That needs Apple's Metal Toolchain, whose download (`xcodebuild -downloadComponent MetalToolchain`, running since about 05:25) appeared stuck at "Beginning asset download" at 07:30.

### Metal Toolchain and precompiled libraries — 22 October

**Install.** `xcodebuild -downloadComponent MetalToolchain` hung at "Beginning asset download" for about three hours. Two things were wrong:
- `mobileassetd` was wedged (`sudo killall -9 mobileassetd` restarted it).
- The network was flagged constrained/expensive (Low Data Mode). In `/usr/bin/log`, the daemon kept answering `MA_QUERY` without ever starting the 838 MB transfer.

After both were cleared, the toolchain installed: build 27A266a, `metal` 32023.921, mounted as a cryptex. Diagnostics that helped:
- `nettop -P -L 1 -J bytes_in -x` showed no daemon traffic.
- `/usr/bin/log show --predicate 'process == "mobileassetd"'` showed the query loop and the network flags. In zsh, plain `log` is a builtin, so use the full path.

**Precompile.** `scripts/precompile_cached_shaders.py --jobs 4` compiled 843 libraries (862 variants, some with identical sources) in 28 s with 0 failures, into `Odyssey_build/build/shader-libs/<sha256 of MSL>.metallib`. The runtime loads an exact SHA match (`shader_lib_prebuilt`) before compiling. `Odyssey.app` bundles the folder (`package_native_app.py`, other session).

**Cold-start measurement.**
- Method: two copies of the shader cache under `Odyssey_build/capture/coldstart/`. Each `*_b.metal` got a unique random comment, so Metal's system cache had never seen them, and each copy started with no pipeline archive. Only the second copy was precompiled.
- Runs: 120 s each through `TOTK_SHADER_CACHE=<copy>`, resuming a copy of `profiles/cap-kingdom-start`. Logs are `Odyssey_build/boot-ab-coldstart-*.log`; the prep script was a one-off in the session scratchpad.

| Run | Shader libraries | Startup critical-pipeline wait | First presented frame |
| --- | --- | --- | --- |
| Cold, no precompiled libraries | 0 loaded, 801 compiled from source | 27.3 s | 31.5 s |
| Cold, precompiled libraries | 801 loaded, 0 compiled | 15.1 s | 20.1 s |
| Second launch, libraries + pipeline archive (797 files) | 801 loaded, 0 compiled | 1.4 s | 5.3 s |

In the second row, the remaining 15 s is the warm-up building 1,225 pipelines from scratch: the archive was empty, and the warm-up waits for every pipeline list row before the first frame. The third row took 797 of its first 1,000 pipelines from the archive. It may have run on the 08:51 binary (another session's fault-reporting and signpost build), so treat it as indicative.

**Title list (09:05–09:10).** `Odyssey_build/capture/title-record/run_record.py` boots with no input to the Resume menu, with warm-up, prediction, async translation and prefetch off. It starts from a cache copy with no collected list, so every title shader and pipeline is built at draw time and recorded: 244 rows (140 shaders, 104 render pipelines), installed as `Odyssey_build/build/pso_title.tsv`. `warm_start` already orders that file first, and the first frame waits only for the first list. Same cold method, 120 s runs:

| Run | Startup wait | First frame | Full warm-up |
| --- | --- | --- | --- |
| Title list, no precompiled libraries | 3.3 s | 5.5 s | 42.6 s, in the background |
| Title list, precompiled libraries | 0.23 s | 1.85 s | 0.6 s |

So the cold first frame went from 20.1 s (precompiled libraries, waiting for all lists) to 1.85 s, and from 31.5 s to 5.5 s without libraries. Re-record the list if the title flow changes, for example with a fresh profile, which shows Start / Assist instead of Resume. `Odyssey.app` must ship `pso_title.tsv` beside its game binary.

## Stall markers and GPU fault flags — verified 22 October

Session 72839a added these; the Verify session (43f9d2) checked them. The harness is `Odyssey_build/capture/verify-fault/run_verify.sh`, which runs `run_ab.py` (a copy of `ab-prefetch/run_ab.py` with the slot owner taken from the environment). Logs are `Odyssey_build/boot-ab-verify-{sp1,spoff,spdef,gpulog}.log` and `boot-ab-signpost-check.log`.

**Flags**

| Variable | Default | Effect |
| --- | --- | --- |
| `DM_SIGNPOST` | off | `=1` turns the markers on, category PointsOfInterest (about 1 µs per interval plus event, persisted to the unified log). Unset or `=0`: one check per marker (about 50 ns), nothing written. |
| `DM_GPU_FAULT_DETAIL` | on | Metal 3 command buffers (`q_cb_new`) use `MTLCommandBufferErrorOptionEncoderExecutionStatus`. A failed buffer logs `【红】[gpu-fault]` and one line per encoder (completed / affected / faulted / pending). `=0` turns this off. |
| `DM_GPU_LOG` | off | `=1` attaches an `MTLLogState` to the Metal 4 main, upload and present command buffers and compiles shaders with `enableLogging`. It prints `[gpu-log] MTLLogState attached…` once, then up to 200 shader log lines. Debug runs only, since it changes compile options. |

**Markers.** Subsystem `org.projectidea.odyssey`. Intervals: `shader-translate` (`totk_shader_rt.m`), and `shader-library`, `pipeline-render`, `pipeline-compute` and `texture-upload` (`nvn_mtl.m`). Events: `present` and `long-frame` (`hle_nvn.c`). Two ways to watch them:
- Without Instruments (works on this Mac): run the game with `DM_SIGNPOST=1`, then run `/usr/bin/log stream --signpost --predicate 'subsystem == "org.projectidea.odyssey"' --style compact > sp.txt`. In zsh, `log` is a builtin, so use the full path. Count the markers with `grep -o '\(begin\|end\|event\)\] [a-z-]*' sp.txt | sort | uniq -c`.
- With Instruments (needs Developer Mode, enabled by the user 22 Oct ~20:00; also `DM_SIGNPOST=1`): `xctrace record --template "Game Performance" --attach <pid> --time-limit 20s --output t.trace`, then `xctrace export --input t.trace --toc` and `--xpath '/trace-toc/run[@number="1"]/data/table[@schema="os-signpost"]'`. `capture/verify-fault/sp_extract.py <export.xml> <out.tsv>` filters our subsystem and resolves the export's id/ref compression. Three caveats: the export lists every event **twice** (same time and id), so de-duplicate. String arguments show as `<private>` because the call sites use `%s`, not `%{public}s`; numbers such as `present`'s frame number are visible. And Game Performance recording roughly halves the frame rate (27 fps while recording, 33 fps during xctrace's ~25 s attach setup, 53–60 fps otherwise), so use it to find where a stall falls, not how long frames take.

**Fix made during verification.** The original header used PointsOfInterest and claimed markers cost one check while nothing records. That was wrong: `os_signpost_enabled` is always true for that category. A 200,000-iteration microbenchmark (one interval plus one event) measured 1.0–1.35 µs with PointsOfInterest, 53 ns with DynamicTracing and 45 ns with `DM_SIGNPOST=0`, and the PointsOfInterest run left 625k entries in `log show`. In Cap Kingdom the game emits about 5 `texture-upload` intervals and 1 `present` per frame, roughly 360 persisted log entries per second during normal play. The 09:04 build made the DynamicTracing category the default, on the assumption that Instruments would switch it on. Once Developer Mode was enabled, that proved wrong: neither `xctrace record --attach` nor `--launch` enabled DynamicTracing, with or without the Points of Interest or `os_signpost` instruments, while PointsOfInterest markers did appear. Since 20:16 (`Odyssey_build/build-verify-fault-2.log`), the default is plainly off and `DM_SIGNPOST=1` turns markers on. Runtime behaviour is the same as the 09:04 build.

**Results (22 Oct, 09:00–09:14).** All runs resumed a copy of `profiles/cap-kingdom-start` and exited 0.
- **Cap Kingdom reached:** screenshots at 55–85 s show gameplay.
- **No new `【红】`.** The only two lines, an occlusion-query note and "pre-laid-out texture storage not in a memory pool", are already in earlier logs. No `[gpu-fault]` lines appeared, so no Metal 3 buffer failed.
- **Markers (`sp1`, `DM_SIGNPOST=1`, 90 s):** `log stream` recorded 24,428 `texture-upload`, 1,280 `pipeline-render`, 11 `pipeline-compute` and 2 `shader-library` intervals, plus 5,023 `present` and 25 `long-frame` events, with every begin matched by an end. `shader-translate` did not fire: the translation cache was warm. It uses the same macro.
- **`DM_GPU_LOG=1` (`gpulog`):** printed the `[gpu-log] MTLLogState attached` line; there were no shader log lines and no faults. It ran at 58 fps over 20–44 s, and a 70 s screenshot shows normal gameplay. It ran about 70 s, not 45, because `run_ab.py` finished its screenshot schedule before checking `AB_SECONDS`. The `verify-fault` copy now stops the schedule at `AB_SECONDS`; the original `ab-prefetch/run_ab.py` still has the quirk.
- **Frame cost.** Frame rate is from `capture/verify-fault/frametime.py` (frame number against time on long-frame lines, 45–88 s):

| Run | Frame rate | Long frames | Over 50 ms |
| --- | --- | --- | --- |
| `sp1` (`=1`, `log stream` listening) | 59.6 fps | 8 | 5 |
| `spoff` (`=0`) | 59.7 fps | 16 | 4 |
| `spdef` (default, then DynamicTracing ≈ off) | 58.8 fps | 23 | 5 |

All three are at the vsync cap, so there is no measurable difference. Expected cost with `=1`: about 6 markers per frame × 1.1 µs ≈ 7 µs, 0.04% of a 16.7 ms frame. The first run of the morning (`signpost-check`, 08:58, 08:51 binary) measured 45.5 fps and 301 long frames. That run came before the other session's pipeline-archive and library warm-up and had a 12.4 s first frame (2–5 s in the later runs), so it is not a marker effect.

**Instruments check (22 Oct, 20:16–20:18, after the user enabled Developer Mode).** This was one 90 s Cap Kingdom run with `DM_SIGNPOST=1` and a 20 s Game Performance attach (`capture/verify-fault/run_xtrace.sh`, log `boot-ab-xctrace-xtrace.log`, trace `capture/verify-fault/xtrace.trace`). It exited 0 with only the two known `【红】` lines, and the 55–85 s screenshots show gameplay. Usable data covered frames 2631–2859, about 9 s. After de-duplication, the trace held 229 `present` events (one per frame), 1,137 `texture-upload` intervals (begin and end counts equal) and 28 `long-frame` events, all in category PointsOfInterest. No pipeline, shader-library or shader-translate markers fired in that window, because nothing new was compiled; those names were seen earlier through `log stream`. **Cross-check:** each of the 28 `long-frame` events maps one to one onto a `[长帧]` log line. Each fires exactly 9 frames after the frame the log line names, because the line is judged once the GPU time arrives. The one log line not matched would have fired at frame 2860, just after the trace ended. So the markers are trustworthy for placing stalls.

### Offline shader seed — coverage, precompile and A/B (22 October, applier session)

The coordinator translated the seed (`Odyssey_build/shader-seed-odyssey`). This session checked it with `scripts/check_seed_coverage.py`, which uses `totk_rt_variant` semantics: same key, `deps_match`, and vertex `depth_minus_w`. The positive control was 862/862; the negative control flagged exactly the faults injected.

- **Coverage with default answers: 446/862 live variants (52%).** Odyssey's binding types are fixed per slot: ps/vs tex9 cube 260/260, ps tex6 cube, ps tex7 mostly cube, ps tex25 / cs tex5 3d, vs attr11 mostly uint. With those per-slot rules plus alternates for tex7 and attr11, coverage is **825/862 (96%)**, the same as the offline simulation predicted. The remaining 37 are rare types (2d_depth, 2darray, 3d/cube in other slots, 1d) or `depth_minus_w=false`.
- **The rule variants are byte-identical MSL for texture rules.** The translator takes texture types from the shader code, so the answer only changes the recorded deps; 20,391 of 22,604 rule variants are duplicates. Only the 2,213 attr11 variants differ.
- **Precompile:** 30,088 unique sources → `build/shader-libs`, 0 failures, 346 MB of data. It took 28.5 min under shared CPU, plus 2 min for the delta. `scripts/precompile_cached_shaders.py --cache <dir> --jobs 8`.
- **Seed size:** 1,028 MB of data (1.2 GB on disk); unique MSL is 579 MB.
- **A/B:** an empty cache (fresh install) with the seed against `TOTK_SHADER_SEED=off`, on the resume route (`capture/seed-ab/run_seed_ab.py`). In-play translations fell from 412 to 24. Without the seed, the 3D world was **black from +50 s to +90 s** after the first frame, even after all translations finished. That is likely one-time render work skipped by async draw-skipping (see upstream `bfdd085`'s deferral exceptions). With the seed, the world rendered normally. The first launch with the seed took 120 s: 32 s verifying and 80 s importing 129,602 files. Upstream `8e53dc5` (whole-folder clone) and `4464e7b` (read the seed in place) address that.

### Seed import speed (task 0f, 22 October, applier session)

The first launch with the seed spent 32 s hashing and 80 s cloning 129,602 files one by one (first frame 119.9 s). `nvn/totk_shader_seed.h` now does the following:

- **Parallel hash check.** Hashes are checked on all cores (`dispatch_apply`, 4,096-file blocks): 32 s → 7.2 s.
- **One folder clone.** The whole seed folder is cloned once into `<cache>.import-<pid>`, in parallel with the hashing, and discarded if any hash fails.
- **Player files merged.** The player's existing entries are cloned over it: `.variants` merged with the player's rows first, the player's files and folders winning, and a different `.metallib` replaced by the seed's. Upstream `8e53dc5` clones only into an empty cache.
- **Atomic swap.** `renamex_np(RENAME_SWAP)` swaps the two folders, so the cache is never missing or half merged. Leftover `.import-*` siblings are removed at the next import.
- **Fast "already installed" check.** It used to take 16.5 s on every start (129,602 `fileExistsAtPath` calls). It is now one `readdir` listing, about 0.9 s; NSFileManager's listing takes 7.7 s for the same 134k entries.

Results:
- CPU timing on the real seed: first import 17.2 s (empty cache) or 19.5 s (played cache); every later start 0.6–0.9 s.
- In game, empty cache: first frame 119.9 → 32.9 s (`boot-seed-ab-seed2.log`).
- What remains: the 12.5 s folder clone (APFS metadata for 129,602 files), and about 14 s in `nvn_mtl.m` `warm_start` (cache listing and planning 50,835 rows) before the gate. That gate reported 0 title shaders ready; reported to the coordinator.
- Probe: `tests/shader_seed_import/probe.m` (16 checks, CPU only).

The same session's independent recheck of the coordinator's `bfdd085` port, on an empty cache without the seed, rendered the world at +50 and +90 s; before, it was black. Dropped draws at frame 900 fell from 1,559 to 171.

### Title list on a seed-only cache (task 0i, 22 October, applier session)

`pso_title.tsv` named runtime variants recorded on a played cache. A fresh install holding only the seed has different variant names for the same shaders, so the gate reported 0 title shaders and 0 pipelines ready. `scripts/seedify_title_list.py` keeps each original row and adds the row the runtime would use on a seed-only cache. It picks the same key and the first seed `.variants` row that passes `deps_match` (plus `depth_minus_w` for vertex shaders). Pipeline (R) rows get their `pso_skey` vs/fs names replaced, and only when each substituted shader is byte-identical MSL (or has identical reflection). The result is 244 → 457 rows: 126 S and 87 R rewritten, 14 S and 17 R unmapped (rare binding types), 0 dropped; all 174 substitutions were identical MSL.

Fresh-install acceptance (seed, empty cache, `boot-seed-ab-seed3.log`): the gate now has 126 shaders and 87 pipelines ready. First frame 24.7 s, partly thanks to the coordinator's `readdir` fix in `warm_start`. In the first 25 s, the worst title-screen frame went from 1,355 ms to 266 ms and on-the-spot pipeline builds from 92 to 11. Rerun the script whenever the title list is re-recorded.

### Replay caches (0a timing) and MetalFX output upscale (0d) — 22 October evening, applier session

**0a, the eight upstream replay caches: inconclusive, no benefit in Bonneton.** Six bounded 150 s runs used `bonneton-cappy` with `run_resume_probe.py explore`: four with the caches off, and on3 and on4 with all eight confirmed `enabled=1` in the logs. A first batch had only `usnap_index` on, because zsh does not split an unquoted `$VAR` into words; it is excluded.

| Metric | Off runs | On runs |
| --- | --- | --- |
| Frame p50 | 16.67 ms | 16.67 ms |
| Frame p95 | 17.6–22.4 ms | 17.7 ms (on4); 27.9 ms (on3, perturbed by `sample`) |
| Submit p50 | 1.6–3.5 ms | 1.9–2.9 ms |
| Instructions per draw | 33–42k | 33–42k |
| GPU p50 | 5.5–6.8 ms | 5.5–6.8 ms |

Logs are `boot-h3-*.log`; scripts are `capture/h3-ab/{analyze,timing}.py`. A 10 s `sample` (`capture/h3-ab/sample-on1.txt`, `sample-on3.txt`) shows Bonneton is not submit-bound. The render thread waits on display pacing, and the encode stage's busy time is mostly its idle `wfe` spin (`nvn_render_stream.h:111–116`). The guest main thread spends about 7% of its samples blocked in `mtl_pool_invalidate → wb_write_list → mtl_finish_gpu`, the H1 writeback drain (task 0b).

**0d, MetalFX output upscale (`TOTK_MFX_UPSCALE=1 TOTK_MFX_OUTPUT=1`): works.** It produced no artifacts, no measurable frame or GPU cost, and slightly crisper UI and text edges on the 3024×1700 window. The run order was off, on, on, off, using `capture/mfx-ab/run_mfx_ab.py` (inputs timed from the first presented frame, binary SHA-256 recorded at start and end), all on one binary.

| Metric | Off | On |
| --- | --- | --- |
| Frame p95 | 19.4 / 17.5 ms | 18.1 / 17.4 ms |
| GPU p50 | 6.8 / 6.6 ms | 6.6 / 6.1 ms |

Logs are `boot-mfx2-*.log`. The camera faced differently after each resume, so only the identical UI could be compared pixel for pixel. The game's GPU metric may not include the present-path output stage. A first 0d batch was invalid: another session's run overlapped it, the binary was rebuilt mid-batch, and fixed-time inputs started a New Game after a late start. That batch is the evidence behind task 0g and the board's no-rebuild-during-batch rule.

### Present latency (0c), freeze watchdog (0k), present margin (0n) — 22 October night, applier session

**0c: conclusive, full screen more than halves input latency.** Eight bounded 150 s runs (`bonneton-cappy`, first-frame-anchored probe, order W W2 F F2 F2 F W2 W) all used one binary. Medians are over 300-frame windows from `scripts/analyze_latency_ab.py`, which now also reports lead and cadence:

| Config | Display lead | Game frame → target refresh | Frame p95 | Cadence p95 |
| --- | --- | --- | --- | --- |
| Window, default | 49.8 / 49.8 ms | 62.8 / 63.1 ms | 20.1 / 20.9 ms | 16.67 / 16.67 ms |
| Window, `DM_PRESENT_LINK=2` | 41.6 / 41.5 | 47.1 / 46.1 | 17.6 / 25.8 | 16.67 / **25.0** |
| Full screen (`BY3_FULLSCREEN=1`) | 16.3 / 16.3 | 29.7 / 29.9 | 17.5 / 22.3 | 16.67 / 16.67 |
| Full screen + `LINK=2` | 16.6 / 16.6 | 22.6 / 22.5 | 18.8 / 20.6 | 16.67 / 16.67 |

- **Noise is small.** Same-config pairs differ by 1.1 ms or less.
- **Where the windowed latency comes from.** In a window, about 50 ms is the compositor's lead. Full screen (direct to the display) removes about 33 ms of it.
- **`LINK=2` is risky in a window.** It juddered (16.67 / 25 ms alternation) in one of the two windowed runs, and was clean in both full-screen runs.
- **Captures.** Window captures confirm the mode: 3024×1898 in full screen, 3024×1700 in a window.

**0k: freeze watchdog, `runtime/freeze_watchdog.c`.** It is off unless `ODYSSEY_FREEZE_WATCHDOG` is set (1 = 5 s, N = N s) and only logs; it never changes the game.
- **What it watches.** Once the first frame has been drawn, it checks every 500 ms whether `dm_nvn_frame_count` or `mtl_present_count` has moved.
- **What it dumps.** On a freeze it logs once and dumps `runtime_dump_threads` (guest threads, core and lock owners) and `mtl_wait_state_dump`. It also spawns `/usr/bin/sample` for 3 s into `ODYSSEY_FREEZE_DIR` (default: next to the profile). It makes at most 3 dumps and logs the recovery.
- **CPU probe** (`tests/freeze_watchdog/probe.c`): a 4 s freeze was caught at the 2 s threshold, the dumps and sample file were written, and the recovery was logged. With the variable unset it is silent.

**0n: per-frame present margin.** `TL_MARGIN` in `totk_timeline.h`/`.c` records, for each display-link present, the µs before its target refresh and the µs from the game's enqueue to that refresh. The coordinator placed the hook in `nvn_mtl.m` after `q4_commit`. Record it with `TOTK_TIMELINE=<file>:<first>:<n>` and read it with `scripts/timeline_margin.py <file> --hz 60`.

**0k/0n in game (21:3x, 0m binary `4a644987`).** With the watchdog unset it printed nothing. A 600-frame timeline gave a margin p50 of 49.82 ms (min 45.49 ms, 0 presents below one refresh) and enqueue → refresh p50 62.87 ms. Both match the 300-frame display lines.

**B1: game time does not depend on the vsync event rate; ratio 1.0.** Odyssey imports `nn::vi::GetDisplayVsyncEvent`, and `frontend.m` signals it at the panel rate (120/s here). `ODYSSEY_VSYNC_DIVIDE=N` (measurement only, off by default) signals every Nth display-link callback. The test used a fresh empty profile with no input, capturing every 0.5 s from the first frame (`capture/b1/run_b1.py`). At 120/s and 60/s, all 12 automatic screen changes landed at identical times, with the Start picker at +13.5 s in both; counting vsyncs as time would have doubled them. Gameplay logs on the 120 Hz panel also show 52–59 game frames per wall second. That was n=1 per arm, but a 2× effect cannot hide under 0.5 s resolution. No change is needed.

## Upstream work not yet ported (next candidates, in order)

1. *(Ported 22 October, see below.)* Pipeline binary archive.
2. **Background translation that is on by default** (`bfdd085`). Upstream's `TOTK_ASYNC_SHADERS` skips deferrable draws and compiles new pipelines on 4 queues. It also fixes translator spawning: an empty signal mask, because SIGCHLD was blocked and spirv-cross hung, plus `POSIX_SPAWN_CLOEXEC_DEFAULT`. Odyssey already has its own `TOTK_SHADER_ASYNC` (translation only, off by default). Compare the two before choosing. Upstream found only 10–20 % of new shaders can be deferred, and the Metal library compile (median 46 ms) is the larger cost, which prefetch addresses.
3. **Patched-variant libraries** (`852f862`) and **border-colour pipelines drawn plain until ready** (`b06c88d`). Both depend on item 2.
4. **Shader seed at build time** (`totk/scripts/prepare_shader_seed.py`, `docs/first-launch-shaders.md`). Translate every shader found in RomFS (BNSH, `.sharcb`, embedded in main) offline, in parallel across all cores (TotK: 166 s → 29 s). Precompile each MSL to `<sha>.metallib`. Warm the title list before the first frame and the rest in the background with `TOTK_WARM_THREADS` (8 threads: 4.6× faster). Odyssey has a 28,284-key inventory and `scripts/precompile_cached_shaders.py`, but no seed yet.
5. **Metal system cache quirk.** For a binary without a bundle, the shared Metal cache only hits when the program's directory path has the same length. Moving the app makes the cache cold, and only one process should write to it at a time. Keep this in mind when comparing timings between `Odyssey Test.app` and the raw binary.

## From other projects and docs (researched, not applied)

- **smo-recomp** (Windows/Vulkan, 1.4.1): translates all 27,888 stages offline and checks them with `tests/check_all_shaders.py`. It runs a scripted Cap Kingdom route with 13 game-state checkpoints (`tests/run_cap_kingdom.py`). Saves use two-phase journalled commits, so HLE `fs` commit semantics must be right or `SaveDataAccessSequence` can hang. Its README still lists first-visit kingdom stalls as unsolved.
- **UnleashedRecomp**: pipelines compile during asset loading rather than at first draw. For Odyssey, prefetch on `nvnProgramSetShaders` is already a step in that direction.
- **Metal 4** (`MTL4Compiler` on your own QoS queues, `MTL4PipelineDataSetSerializer`, `MTL4Archive`, unspecialized pipelines with `MTLPixelFormatUnspecialized`): compile a shader body once and specialise it per render-target format. This is a later phase, after the binary archive.
- **Texture upload**: Apple-silicon Macs sample BC and ASTC natively. The renderer already uploads them natively, so CPU `nvn_bcdec.c` only runs on GPUs without BC. Deswizzle already runs on the GPU (`tex/nvn_deswizzle_gpu.m`). The remaining 150 ms–2.3 s first-upload cost is "preparation"; next, find out whether it is first-use compute pipeline creation for the deswizzle/transcode kernels (they could be prewarmed at startup) or per-texture work. References: yuzu `FastProcessBlock`, Rust `tegra_swizzle`, `MTLIOCommandQueue` for streaming from a local cache.
- **Attributing stalls**:
  - `xcrun xctrace record --template 'Time Profiler' --attach <pid> --time-limit 20s --output t.trace` (needs full Xcode, which is installed).
  - `sample <pid> 5 -file out.txt` during a stall.
  - `os_signpost` intervals around compile, upload, `waitUntilCompleted` and RomFS/Yaz0 reads.
  - Upstream `TOTK_LONGFRAME_DETAIL=1` and `TOTK_TIMELINE`, both in this tree.
- **GPU panic hygiene**:
  - Run debug sessions with `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` (`MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1`).
  - Set `MTLCommandBufferDescriptor.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus` to log encoder faults.
  - Watch `currentAllocatedSize` against `recommendedMaxWorkingSetSize`.
  - SPIRV-Cross MSL has no robust-buffer-access option, so bounds checks would have to come from the translator.
- **Getting past the opening** (OdysseyDecomp is 1.0.0, so every symbol must be re-verified against 1.3.0):
  - `GameDataFile::mIsPlayDemoOpening` and `GameDataFunction::isPlayDemoOpening()` gate the opening.
  - The skip path runs through `IUseDemoSkip::isEnableSkipDemo`, `StageSceneStateSkipDemo` and `PlayGuideSkip`.
  - Saves are `File%d.bin` and `Common.bin`.
  - Options: automate the skip prompt; keep a local first save (never in Git); or, for debugging only, force `isPlayDemoOpening()` false after verifying the address.

Sources: upstream commit messages (`git -C MacNativeGames log 43f53b4..origin/main`), `MacNativeGames/docs/{first-launch-shaders,pipeline-prediction-stalls,performance-followups}.md`, https://github.com/Coz-0/smo-recomp, https://github.com/hedge-dev/UnleashedRecomp, https://developer.apple.com/documentation/metal/mtldevice/shouldmaximizeconcurrentcompilation, https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api, https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Configuration_Parameters.md, https://github.com/MonsterDruide1/OdysseyDecomp, local `man MetalValidation`.
