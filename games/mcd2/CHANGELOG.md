# Minecraft Dungeons 2 port status

## 2026-10-28 — particle compute shared-memory candidate (not packaged)

- Walking-dust investigation traced repeated/sentinel particle indices back to
  radix-sort dispatches. Captured 19-entry key/index streams fail pair-multiset
  preservation on all five inspected passes; this is not merely alpha filtering.
- Generated sorting MSL declares 4096 shared bytes but accesses offsets beyond
  8 KB. Guest control capture at +0x754 reports 9444 bytes for sorting, 8836 for
  histogram and 5744 for prefix scan. The SDK has accessors reading that field
  next to the three compute local-size fields. Runtime requests omitted it, and
  the service also omitted forwarding it; Ryujinx selected its 4096-byte fallback.
- Candidate now forwards guest shared bytes, requires explicit aligned metadata,
  records a cache dependency and includes it in request hashing. Legacy compute
  variants cannot satisfy the new request; a service which fails to preserve the
  dependency is rejected rather than publishing the old fallback translation.
- CPU reference/audit artifacts and real-shader retranslation are in the external
  work directory. All three regenerated MSL arrays match the captured byte sizes;
  missing metadata is rejected. Runtime and candidate translator build successfully.
- User reports the effect looks substantially better and requested commit/push.
  This is subjective feedback on the running build, not an A/B validation of the
  new shared-memory candidate. GPU sorting equivalence, complete driver-input
  replay and candidate walking-dust visual validation remain pending.
  This is NOT a dust-fix delivery. Formal App and
  repository translator prebuilt are unchanged; candidate service is separate.
  MCD1 remains frozen. No save/cache merge, opacity override or cache-source edit.

## 2026-10-28 — first-use action shader compile target

- Added per-job queue/translation/Metal/publication-lock timings and opt-in exact
  request capture (`TOTK_SHADER_CAPTURE_REQUESTS=1`). Gameplay captured 503 jobs;
  slow action shaders spent 1.1–1.2 s in Metal, versus ~45–62 ms translating.
- Explicitly compile translated shaders as MSL 2.3, matching the service's
  `--msl-version 20300`, instead of selecting the OS default dialect. Keep safe
  math, normal optimization, descriptor bounds and mandatory draws unchanged.
- Same-source Metal API probes on eight samples: default 708–1102 ms, MSL 2.3
  117–162 ms. Four other samples tested in reverse order: default 589–704 ms,
  MSL 2.3 71–170 ms. These are library measurements, not whole-frame speedups.
- Copied-profile gameplay with a separate initially empty translation cache:
  279 jobs, maximum Metal time 177.69 ms and maximum logged shader wait 246.78 ms.
  User takeover stopped automatic input; session ended on the window-close path.
  Routes/durations differ and the system compiler cache is shared: not controlled
  gameplay A/B. Multiple new pipelines still caused a 1315.4 ms frame (2150),
  including 705.2 ms for seven pipelines / thirteen libraries. Two startup frames
  also exceeded one second. First-use stalls are reduced in one component, NOT
  resolved end to end; continued pipeline/batch preparation work is needed.
- Rejected and removed an early program-creation translation experiment: recent
  binding hints can be wrong and add an extra ~1.1 s compile. Also tested size
  optimization and translator AST optimization; neither removed the bottleneck.
- Production compile-options regression passes. All 11 descriptor regressions,
  including MSL 2.3 GPU readback at 524288 textures / 4096 samplers, pass. Four
  MCD2 CPU regressions pass. Broader Dungeons suite: 36 tests, seven opt-in skips,
  no failures using the project environment (system Python lacks cryptography).
- Only MCD2 changed. MCD1 remains frozen; no formal save/cache merge or cleanup.

## 2026-10-28 — command recording leak / memory-watchdog exit

- The user-controlled baseline test exited at frame 38100 after roughly 20 min:
  resident memory 24931 MB exceeded the unchanged 24 GiB watchdog. No new ObjC
  exception; 380000 command copies remained live, occupying 22268.5 MiB.
- Restored the registered OUT_OF_CONTROL_MEMORY callback before EndRecording
  publishes a handle. The game supplies control storage lazily; without that
  callback all copies were indexed at zero and could never retire on control
  reuse/token invalidation. Callback runs without copy/index locks; validate its
  result and use subtraction-based remaining-space checks. No age-based freeing
  or higher allocation cap. No-callback callers retain existing behavior.
- Candidate actual gameplay: at 100000 recordings only five copies / 0.1 MiB
  live; 99297 retired at control reuse and 698 by token retirement. Resident
  memory was 2763 MB at frames 7800 and 10800. Scripted input stopped on user
  interaction. The separate-profile session ended after frame 11400 with
  `window closed; graceful guest shutdown pending`; no newer macOS crash report
  was found. This records the window-close path, not a verified shutdown exit
  status. This is shorter than the failed run; long-session validation remains
  pending.
- Focused regressions: 32 tests, four optional tests skipped, others pass. Added
  production callback tests for lazy supply, exact remaining space, exhaustion,
  overflow, missing/undersized callback results, and legacy no-callback behavior.
- Packaged MCD2 only with this lifecycle correction and required-stage scheduling.
  First-use shader stalls remain visible and are not declared fixed. MCD1 frozen.

## 2026-10-28 — required shader scheduling

- Latest gameplay report confirms level entry no longer crashes, but first-use
  actions/objects stall. Reported frame 2148 took 1689.7 ms: three shader requests
  took 1395.98 ms, GPU 20.3 ms, and no texture upload was recorded.
- Required shader misses now use the urgent single-flight workers and wait on
  the condition variable with the global cache mutex released. Queued jobs are
  promoted rather than reclaimed for translation under that mutex.
- Queue the current draw's unresolved vertex/fragment specializations before
  waiting for either stage. Preserve binding state and all existing mandatory
  compute, memory-write and occlusion work; no broader draw-skipping policy.
- Extracted-production regressions cover parallel required jobs, released cache
  mutex, duplicate requests, queued-job promotion, failure wakeup/suppression,
  graphics/compute stage preparation and unchanged shader bindings.
- Candidate runtime built; initial focused suite: 31 tests, four optional tests
  skipped. User takeover stopped scripted baseline input; that separate-profile
  session later exited at the memory watchdog (see control-memory fix below).
  Candidate gameplay still has >1 s first-use shader waits: no claim of eliminated
  stalls or controlled performance improvement. No concurrent GPU runs.
- MCD1 is frozen at the user's request: no source or App changes for this work.

## 2026-10-28 — retained texture crash and movie queue ordering

- User level-entry crash identified as `-[NvPkg swizzle]`: cached texture
  representation disagreed with the upload policy inferred from a guest-owned
  pool object. Both renderers now dispatch retained cache entries by actual
  object class, preserving the underlying texture (or nil if evicted), and
  avoiding the reverse unsafe NvPkg ivar cast as well. The pool lifecycle that
  changed the inferred policy has not been independently captured.
- H.264 host pacing sorts already-buffered pictures by presentation timestamp,
  leaving EOS last; late-frame checks guard future/equal/reversed deadlines.
  This does not implement a full codec reorder/delayed-output drain.
- Added extracted-production regression tests for both game copies: retained
  package/plain texture policy changes, evicted packages, reordered PTS, EOS,
  AAC bypass, late drops and unsigned timestamp subtraction.
- Added slow shader request logs including lookup, translation, lock and worker
  waiting, which were absent from the existing Metal library/PSO timing.
- Focused regression suite: 29 tests, 4 optional tests skipped, no failures.
  Both native runtimes rebuilt successfully. Copied-profile startup checks pass.
- MCD2 copied-profile entry probe reached actual level gameplay/HUD by 65 s
  and continued beyond 3500 frames without the reported exception. Several
  300-frame windows have median frame time ~16.67 ms. First-use synchronous
  shader requests still take 0.4–0.7 s each and cause multi-request stalls.
- Startup H.264 probe reports zero late drops after queue ordering (previous
  reported run: 22). This is not a controlled cutscene performance comparison.
  Full-level stability, the reported blue cutscene imagery, and MCD1 first-use
  stalls remain unresolved; no save-profile merge or cache cleanup performed.

## 2026-10-28 — startup audio regression correction

- Fixed renderer request handling of unused, zero-filled voice records. Their
  default ID zero previously reset the active movie voice after each update,
  preventing the played-sample counter from advancing and stalling startup.
- Active IDs are updated independently of wire order; absent voices are retired
  only after the entire request has been processed.
- Extended the production audio probe with zero-filled unused entries before
  and after active voice zero, consecutive requests without buffer resubmission,
  sample-counter continuity, and absent-voice retirement. Both game probes pass
  46587 checks. Dungeons Python regressions: 33 tests, six optional tests skipped.
- MCD startup test reached the Chinese camp-loading screen; the user took over
  that test window, so it was left running with its separate copied profile.
- MCD2 diagnostic run released its startup movie decoders and continued past
  900 rendered frames. A subsequent run with the normal shader cache reached
  the Chinese Start Game menu (frame 1235 screenshot). The user interacted with
  its copied-profile test window, so it was left running. Full level/cutscene
  validation remains pending.

## 2026-10-28 — media, pool textures, and presentation update

- Added host H.264/NV12 and AAC/PCM16 decoding using VideoToolbox and
  AudioToolbox; retained VP9 support. Generated-media decode/flush tests and
  a real game movie backend probe passed. Full guest cutscene playback remains
  unverified, including reordered video timing and AAC priming/tail behavior.
- Added direct-indexed 2D texture-pool reflection and Metal argument encoding,
  preserving observed descriptor capacities and sparse IDs. The previously
  failing shader code translates and compiles with Metal. Synthetic Metal 4
  readback passed at 524288 texture / 4096 sampler capacity; this is not an
  end-to-end gameplay test.
- Translator-reported variant failures now log and skip unavailable programs
  rather than aborting the process. Service launch/transport failures remain fatal.
- FIFO presentation now selects the oldest queued frame, and repeated
  application-environment messages log only once per process.
- Both apps rebuilt and signature/hash verified. Regression run: 33 tests,
  one optional specialization test skipped; 64 translator checks passed.
- Remaining work: in-game level/cutscene validation, frame-time measurement,
  and profiling of full-pool argument encoding overhead.

## 2026-10-26 — experimental integration

- Added a `GameBuilder` subclass, pinned three-module rewriting and verified base/update RomFS merge.
- Integrated optional DLC with title checks, six-level IVFC verification and pinned image hashes.
- Use host secure randomness; networking is deliberately offline.
- Corrected launcher storage identity; app data and saves are isolated from other games.
- Startup enters `nnMain` and reaches native Metal initialization. Rendering and gameplay are not yet validated.
- Known gaps include sparse texture storage, movie decoder formats, and SDK calls discovered during boot testing.

## Draw-parameter implementation (GPU validation pending)

- Implemented vertex-driver `c0+0x640/0x644/0x648` as base vertex/first,
  base instance and multi-draw index. Metal builtin offsets remain unchanged.
- Driver-bank cache keys and argument generations now include draw changes.
  Replay metadata follows deferred draw copies. Indirect driver parameters are
  copied on the GPU timeline from resolved draw arguments, not stale CPU memory.
- The read validator accepts only the three known vector components; unknown
  fourth-word, full-vector, dynamic and oversized reads remain errors.
- Added CPU-only validator coverage and extended `probe-gpu-drv-c0` with direct,
  indexed, multi-draw and GPU-written indirect parameter checks.
- Native compilation and CPU tests pass. The GPU probe exited during guest
  address-space reservation before reaching its checks. No new boot/rendering
  result is claimed; further GPU tests are deferred while another game runs.
