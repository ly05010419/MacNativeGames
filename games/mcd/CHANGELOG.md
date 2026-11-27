# Minecraft Dungeons port status

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
  900 rendered frames. Full level/cutscene validation remains pending.

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
