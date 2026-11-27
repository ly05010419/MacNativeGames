# D3 uniform overlay performance — 2026-10-29

## Evidence

Historical D3 logs contain sustained 121.76 ms median frames (117.28 ms
submission/presentation, 3.51 ms game GPU time), including long frames with
no shader/pipeline compilation or texture uploads. These windows have not been
mapped conclusively to a particular Rift scene.

A passive CPU sample of the unchanged packaged runtime found 669 of 763 samples
under queue submission in memory copies reached through
`encode_args -> upd_shadow -> pend_overlay_x`. The forward overlay recopied all
unfinished writes, including bytes completely replaced by later inline updates.
Repeated per-object updates therefore produce quadratic copy volume.

## Candidate

D3-local `pend_overlay_x` walks pending writes newest first and copies only
uncovered destination bytes. It does not modify the pending log, snapshot prefix,
fences, arena ownership, draw order, shaders, or effects. Fragmentation beyond
32 local uncovered ranges, or a relevant GPU-copy entry, falls back to the
original ordered implementation using the same sampled completion boundary.
No updates are dropped or ranges truncated. `DM_D3_PEND_PRUNE=0` selects the
original overlay for A/B testing (set before launch).

## Validation

- `python3 -m unittest discover -s tests -p test_d3_pending_overlay.py -v`:
  production functions compiled with AddressSanitizer/UndefinedBehaviorSanitizer;
  12,000 randomized byte-equivalence cases plus boundary, completed-fence,
  snapshot-prefix, fragmentation, recursive GPU-copy and depth-limit cases pass.
- Synthetic repeated-update fixture checks every version of a 1 KiB block:
  2,048 updates require 2,148,532,224 overlay-copy bytes before versus 2,097,152
  after. This is a copy-volume result, not a gameplay speedup claim.
- Development runtime builds successfully.
- `probe-gpu-ub-rename`: 1,474 checks pass.
- `probe-gpu-ubo-snapshot`: 16,408 checks pass, including its intentional
  inaccessible-page fault-guard case.
- Isolated candidate gameplay session, copied profile/cache: multiple 300-frame
  windows have median frame time 16.66–16.68 ms, submission 2.58–2.90 ms,
  approximately 612–700 draws/frame. Earlier unchanged-runtime session had
  44.36 ms frames / 42.45 ms submission / 675 draws/frame in its last window.
  Workloads are similar in draw count, but NOT a controlled identical-scene A/B:
  user navigation and presentation settings changed in the earlier session.
- Candidate CPU sample no longer has dominant historical-overlay copying.

Artifacts are under external `D3_build/logs/crowded-scene` (baseline, build,
GPU probes) and `D3_build/logs/crowded-candidate` (candidate log, TSV, CPU sample).
The user took over both gameplay sessions; automated input stopped on detection.
The original session closed through its window; the candidate was left running.

## Delivery / remaining checks

Only the development runtime was built. The formal App, prebuilt translator,
and formal saves were not changed. No commit/push performed for this change.
User gameplay feedback after taking over the candidate session: with the frame
rate set to 120 FPS, Nephalem Rift combat involving dozens of monsters and many
simultaneous effects ran at 80–120 FPS, mostly 110–120 FPS. The user described the
improvement as very obvious. This covers the originally reported high-density
combat scenario, but is user-observed performance, not an automated identical-scene
A/B measurement or a frame-time percentile analysis. It does not establish that
all scenes maintain 120 FPS. Controlled visual comparison and long-session
validation remain pending. Other games are untouched.
