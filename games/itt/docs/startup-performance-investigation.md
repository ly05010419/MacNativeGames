# ITT startup / two-player performance investigation

## Evidence (2026-10-29)

Preserved formal logs: external `ITT_build/logs/startup-investigation/original.log`
and `original-crash.log`.

- First presented frame is ~0.7 s, but can be black: the successful session later
  blocks 19,794 ms at shader warmup. 943 libraries compiled from source, zero
  precompiled loads, 944 pipelines. The App has no `build/shader-libs` directory.
- The preceding run crashed at guest main+0x3509b90, `str w9, [x8]`, x8=3,
  w9=0xe0000002. The caller explicitly chooses this fatal path for result -2.
  At main+0x350a06c..0x350a088, elapsed-time comparison can produce -2.
  This is evidence of a guest timeout/fatal path, not proof that shader warmup
  caused it. Do not remove the crash instruction, disable the watchdog, or raise
  timeouts to hide the cause. Exact triggering state still needs capture.
- Movement-era windows include GPU medians ~8–10.7 ms and p95 ~12 ms, unlike D3's
  CPU-copy bottleneck. Other windows have 30–60 ms frame spikes without matching
  submission/GPU cost. Need gameplay-thread and GPU-pass sampling.
- Rendered FPS and presented FPS need separate validation: the historical log
  switches display-link mode 60 -> 90 -> 120 but later records 16.67 ms display
  intervals and many merged queued frames. No cause established yet.

## Candidate preparation

ITT-local `scripts/compile_shader_libs.py` integrated through existing Makefile
`shader-tools`, preserving `IttBuilder` / flat work layout. Reads original
recorded ITT shader sources with matching chain/key plus content-checked variants;
compiles with safe math, names libraries by exact source SHA-256, validates
compiler/options and library digests before reuse. Atomic compiler output,
failed-build cleanup and manifest-only installation retain runtime fallback.
No cached MSL/reflection rewritten, no game effects removed.

Production helper compiled 653 unique sources (954 source records in the initial
inspection) successfully. CPU fixture tests exercise reuse, corruption, compiler
identity changes, failure cleanup, source preservation and installation checks.
This is not yet a measured startup improvement or crash fix.

Formal App is unchanged. No ITT commit/push yet. Existing staged rewrite changes
in `third_party/d2r/patch_nso.py` and unrelated staged work were left untouched.

Prepared isolated launcher: `/tmp/itt-startup-test.py`, copying formal profile and
cache to analysis folders and using the unchanged development binary (SHA-256
matches the formal runtime) with newly compiled libraries beside it. Launch was
refused because a user-owned Diablo III session was running; no game terminated.
Next: run when GPU/game slot is free, measure warmup/precompiled load counts and
repeat startup stability, then profile two-player movement. Stop all automated
input immediately upon user takeover. Never merge test saves into formal saves.

## Live validation after D3 exited

- First isolated launch (PID 97765), unchanged runtime plus precompiled libraries:
  reproduced the same guest main+0x3509b90 / fault 0x3 / result -2 crash while the
  synchronous historical pipeline-list barrier was still waiting. Precompiled
  loading alone is insufficient. Log: `logs/startup-investigation/precompiled-run.log`.
- Candidate changes `warm_start` to wait only for an explicitly supplied manifest
  or a first `pso_title.tsv`. Whole-session collected lists remain in the background.
  `mtl_warm_wait` likewise does not turn speculative historical work into a world-entry
  barrier. Existing required shader/pipeline resolution and guest timeout logic
  remain intact. No draw-skipping policy is enabled by this change.
- Three CPU regression tests pass; runtime build succeeds.
- Second launch PID 304, `logs/startup-nonblocking`, explicitly sets
  `TOTK_ASYNC_SHADERS=0` to rule out skipped draws as the reason for faster startup.
  Startup barrier logged 1 ms; background warmup 1,979.8 ms; 952 precompiled library
  loads and 0 source compiles. Screenshot at ~12 s shows the title screen.
  Prior formal run waited 19,794 ms, but caches/path/session differ: not a strict
  cold-cache identical-scene A/B. One successful launch does not prove all crashes fixed.
- User keyboard takeover detected; no further automated input was sent. Session
  left running. Only passive CPU samples and log analysis followed.
- Frames 3000–17483 in the live session: frame median 9.307 ms / p95 12.165 ms,
  GPU median 8.755 ms / p95 11.082 ms; submission CPU median 3.812 ms.
  This mixed window includes level-loading spikes (compilation/uploads), not a
  controlled two-player movement benchmark. Render passes median 160/frame.
  Displayed-frame intervals remain ~16.67 ms despite requested 120 Hz: further
  presentation diagnostics are needed before claiming true 120 FPS output.

Formal App remains unchanged, test saves remain separate, no commit/push performed.
Current candidate must remain available for user testing; do not terminate or
restart it after takeover. Pending: repeated startup validation, actual display
callback timing, GPU per-pass costs, and movement-specific scene comparison.

## User feedback and death-effect diagnostics

The user reported no obvious gameplay performance improvement and pronounced
mosaic-like death effects. This is NOT a validated movement-performance success.
No representative death screenshot or implicated shader is available yet; particles,
dissolve/discard and post-processing remain hypotheses, not established causes.
The previous user-played session ended with a window-close/graceful-shutdown log.

Added an opt-in `ITT_EFFECT_CAPTURE_DIR` diagnostic trigger to ITT's real keyboard
entry: F9 (Mac keycode 101) requests four frames of pass logs, a texture dump, and
12 consecutive presented-game-frame screenshots. Held-key repeat is ignored and
captures have a ten-second cooldown. No capture is triggered by synthetic input.
The directory must exist; printf-format characters and overly long roots are
rejected. Without the environment variable there is no capture. Rendering rules,
shader source, reflection, filtering and blend/discard behaviour are unchanged.

CPU fixture tests exercise the actual trigger with stubbed graphics callbacks,
including opt-out, unrelated keys, repeat/cooldown and invalid paths. All four ITT
tests pass and the native runtime builds. This does not validate live GPU captures
or repair the artifact. Initial fixture call-count expectation and a missing early
screenshot declaration were corrected before the successful build.

Started diagnostic PID 10729 using another copy of the prior test profile/cache,
under `analysis/effect-test-profile` and `analysis/effect-test-cache`. Logs and
captures go to `logs/effect-capture`; no automated movement was sent. Launcher
checks for other games first. `DM_NVN_TEXSNAP_MIN=128` additionally preserves
intermediate attachments during the requested texture-dump frame; constant-buffer
capture is enabled for requested pass-log frames. These readbacks can stall and
must not be used for FPS comparisons. The requests can land on adjacent frames:
correlate actual logged frame numbers, not just the common directory name.

Formal runtime hash remains
`6cb0c91effa266a324ecf4e4920a0719e06ebce5c65ddf00aece57405ff845db`.
No formal saves, App files or staged unrelated work were changed; no commit/push.

## Follow-up: concrete compute translation defect, visual attribution pending

The user again reports mosaic mixed into death effects. The diagnostic session
closed gracefully; its captures/buffers folders are empty and no F9 capture trigger
was logged. Thus there is still no death-effect image or draw/pass attribution.
Do not present the diagnostic-only build as an attempted rendering fix.

Read-only inspection of the actual ITT test shader cache found 93 compute variants
with a 1024-element uint Workgroup array. Four variants (two shader keys beginning
`63f057ee` and `e15d15ad`) contain constant accesses at indices 1028 and 1029,
including initialization, atomics and reads. This is outside the declared array.
The defect already exists in cached SPIR-V, before SPIRV-Cross/MSL and offline Metal
compilation; it is not merely a textual MSL interpretation. The runtime's current
translation request does not forward the compute shared-memory byte declaration.
The exact guest declaration for these ITT shaders has NOT yet been collected, so
no allocation size has been guessed, no cached sources/reflections were patched,
and the MCD2 candidate has not been copied into ITT as an unverified fix.

Added `scripts/audit_compute_bounds.py`: read-only parser for direct constant
Workgroup-array accesses. It explicitly does not prove dynamic/nested/aliased
access bounds or identify which visible effect uses a shader. Six ITT tests pass
including two new synthetic auditor tests. Audit of 970 cached SPIR-V modules:
32 offending access instructions across four modules, zero parse errors. Exit 1
is the expected indication of findings, not an audit crash. Evidence is in
`logs/effect-capture/compute-bounds-audit.json` and comparison against the formal
shader cache in `compute-bounds-baseline.json`.

Next: obtain the original NVN compute control declaration for the affected keys,
then validate a matching metadata-aware translator/runtime and cache invalidation.
Death-frame attribution and visual regression remain required. No new game session
was launched in this follow-up; formal App/saves and unrelated index are unchanged.

## Exact guest metadata and matching candidate (follow-up)

- Control-collection session PID 20669 used isolated profile/cache copies and
  `TOTK_CTL_DUMP`. User controller takeover was observed, no subsequent automated
  input sent. The window was subsequently closed gracefully by the user.
- Captured 24 distinct compute keys. Guest shared declarations include 0, 4, 172,
  512, 560, 816, 1584, 1840, 2096, 2608, 4964, 8192, 8836 and 9444 bytes. Shader
  identity was verified by SHA-256 of the captured control prefix plus the exact
  cached guest code before offline retranslation. The two keys with literal
  index 1028/1029 were NOT encountered; their exact declarations remain pending.
- Added ITT-local translator sources, runtime shared-byte forwarding before cache
  lookup/request hashing, symmetric shared dependency matching, and reply witness
  validation. Legacy 4 KiB compute variants no longer satisfy requests. Explicit
  zero remains valid. No guessed allocation size or modified cache metadata.
- Initial translator source fork from current TOTK failed graphics compatibility
  checks (unrelated vertex-output initialization and depth/dependency changes).
  Rebased ONLY newly created ITT translator files onto historical 947d4c0, whose
  prebuilt hash matches the inherited ITT service. Four baseline patches, unchanged
  vendor source. This corrected candidate matches 16 sampled graphics translations
  against old service and cache (SPIR-V/MSL bytes, parsed reflection JSON).
- Five invalid shared metadata requests rejected (missing, negative, unaligned,
  string, null). 24 real compute translations preserve exact shared bytes, pass
  the limited constant-index bounds audit, and all compile using Metal safe math.
  Dynamic accesses are not proven. Eight ITT regression tests pass; runtime builds.
- Artifacts: `analysis/compute-metadata-retranslation/{report.json,metal-compile-report.json}`,
  `analysis/translator-validation/report.json`, `logs/compute-control/`.
- Started matching runtime/service candidate PID 37013, `logs/compute-memory-candidate`,
  with copies under `analysis/compute-memory-test-{profile,cache}`. Development
  service backed up to that log directory as `previous-totk_xlate` before replacement;
  repository prebuilt/formal App untouched. Initial live cache shows witnessed new
  compute variants (including 8192 bytes), confirming old variants are rejected.
  Candidate remains `TOTK_ASYNC_SHADERS=0`. No automated movement was sent.

Important release blocker: repository prebuilt is STILL OLD. Do not package this
runtime with that service. A validated matching prebuilt must be delivered together,
only after visual validation/authorization. Startup/performance/death effects are
not declared fixed; this is an actual compute-metadata correction candidate.

## User: death now has smoke, maximum roughly 80 FPS

User reports the death effect now appears to have smoke rather than the previous
mosaic, but maximum FPS is around 80. This is a visual change, NOT approval of
correct rendering or performance. PID 37013 closed gracefully; no games were
running when checked. Three actual user F9 captures now exist (2.4 GB total):
frames 24637/24638, 26732 and 34281/34282. They visibly show death fragments with
surrounding smoke. Capture timings/readbacks must be excluded from benchmarking.

Complete per-frame TSV, not the slow-frame-only sample: candidate frames
28000–32000 (4001 frames, well away from captures) have frame p50 11.630 ms,
p95 12.412 ms; GPU p50 11.243 ms, p95 11.571 ms; submission CPU p50 5.221 ms.
No new libraries, pipelines or texture uploads in that interval. This establishes
sustained GPU-side cost rather than ongoing compilation as the explanation for
that window's ~86 rendered FPS. It does NOT establish which pass causes the cost.
120 FPS allows 8.333 ms. Actual presentation cadence still reports 16.67 ms,
a separate unresolved issue (rendered frame rate is not displayed frame rate).

Earlier mixed-session samples have ~8.7 ms GPU medians but different workloads:
previous candidate ~160 passes/814 draws versus current stable window 223 passes/
1403 draws. Thus cross-session differences do not prove the shared-memory fix alone
causes the slowdown. The initial frames 3000–15000 in this latest run were only
37 passes and 60-Hz cadence, not comparable gameplay. Full statistics saved as
`logs/compute-memory-candidate/performance-comparison.json`.

Contact sheets and actual pass-map inspection now locate visible smoke/fragments
in F26732's intermediate 1248x702 attachment ser156413, pass1615959 (relative79),
fragment shader `7cf1a356...-a6d3c6c2064f_b`, vertex `142df288...-9cdc36cd9069_b`.
It composites into 1920x1080 ser139429 in pass1615960 using fragment
`362baeb1...-45165dbe1085_b`. These are first inspected appearances in this snapshot
sequence, not proof that the shaders are faulty or direct proof of compute causality.
The two previously identified fixed-index OOB keys were still not recorded in the
session's control dump. No filtering/blending/discard changes made.

Prepared/started PID56419 under `logs/compute-memory-gpu-profile` using another
copy of the last test profile/cache, unchanged runtime/service/graphics settings.
Sparse `TOTK_PASS_GPU=2000:60000:600` collects Metal4 encoder timings every 600
frames (starting at2000); those sampled frames are diagnostic and excluded from
normal FPS comparisons. `TOTK_PASS_GPU_FILE` stores all encoder rows. No automated
movement sent; stop all automated input upon user takeover. The older
`DM_NVN_PASSGPU` render hook is disabled for Metal4, so it was NOT used.
No formal App/profile changes, commits or pushes in this follow-up.

## User: no other observed problems; focus remains 120 FPS

User now reports “现在除了帧率达不到 120，没发现其它问题。” Preserve the
shared-memory correction and current effects. This is not comprehensive graphics
validation or ITT release authorization. Sparse profiling session PID56419 ended
with graceful window-close logging; no game processes were found at the subsequent
check. No new gameplay session was launched for this analysis.

Nine sampled frames 9200–14000 rank aggregate encoder groups as: unnamed compute
27.99 ms; render 1920x1080/format92/first-key0893211e 20.48 ms (278 encoders);
unnamed blit 15.31 ms; format80/081cf97f 13.26 ms (92); format80/abaff555
13.03 ms (296); format80/def2bc12 8.59 ms (93). These are sums over diagnostic
frames, NOT normal per-frame costs or potential savings. Timestamp barriers change
overlap; first shader prefixes do not describe every draw in an encoder. Evidence:
`logs/compute-memory-gpu-profile/gpu-group-summary.json`.

### Concrete split cause found in existing captures

Read-only audit of all twelve passdump frames finds **824** explicit
`绘制采样本通道画过的附件，先写回再画` boundaries (53–95 per captured frame).
All 824 immediately follow a logged draw with `fs=- dw=0 st=1`; the next recorded
pass samples the same depth attachment, through an actual Metal format260 view
(Depth32Float_Stencil8). Example: F26732 passes1615906→1615907→1615908,
with a stencil-volume draw between fragment draws using def2bc12/abaff555.
The hot 0893211e and 081cf97f groups also bind that depth texture.

Source explains a plausible false dependency:
- `g_sd` distinguishes depth (`depth=1`) from stencil (`depth=2`).
- A non-sampling stencil draw marks the stencil entry in `g_sd_plain`.
- `draw_att_reads` returns texture serials, with no sampled-plane information.
- `att_unstored_read` matches those serials against BOTH entries. A subsequent
  depth sample therefore also sees the stencil write and ends the encoder.

This establishes the logged split path and a concrete optimization candidate, not
permission to remove the guard. Need exact sampled-plane classification, conservative
handling of image access/aliases/unknown views, depth writes and pending clears,
and GPU equivalence checks for concurrent depth sampling/stencil updates before
changing rendering behavior. Do not turn off TOTK_ATT_STORE or queue barriers.
No cached MSL/reflection modification or effect reduction is involved.

Earlier `captured-pass-map.json` counts used uppercase `D` (first draw summary),
so “one draw” there is NOT an actual per-encoder draw count. New audit counts
lowercase `d` records, retains the preceding draw state and distinguishes missing
next-pass evidence. GPU TSV draw deltas remain a separate source.

Added `scripts/audit_attachment_splits.py` plus three synthetic regression tests;
all 12 ITT tests pass. Full result:
`logs/compute-memory-candidate/attachment-split-audit.json`. The auditor is read-only
and does not infer FPS savings or claim dependency correctness. Current runtime,
service, formal App and saves remain unchanged in this follow-up; no commit/push.

## Opt-in depth sampling continuation candidate

Added `probe-gpu-depth-sample`: synthetic Metal4 rendering through production
encoder wrappers, 32 alternating stencil-update/depth-sample pairs, scissored
regions, stencil equality tests and masked stencil replacement. It first stores
the initial depth clear, then compares split and combined encoders. Color, depth
and stencil readbacks match byte for byte and 12,288 expected-value checks pass
on Apple M1 Max with Metal API Validation enabled. Negative control deliberately
omits sampling draws in the combined case and aborts at the expected pixel mismatch.
The fixture forces direct encoding (render-stream off); it does NOT validate the
production resource-classification guard, guest shaders or full gameplay.
Artifacts: `logs/depth-sample-probe/`; pre-change runtime backed up there.

Implemented `ITT_DEPTH_SAMPLE_CONTINUE=1`, default OFF. It only overrides an
existing att_unstored_read boundary for a single sampled serial that is exactly
the current depth target: loaded depth never written in this encoder, loaded
stencil, no current depth write/export, no mid-pass depth clear, no combined-pass/
layered/fused path. Requires resolved, current-generation, non-stencil,
non-format-overridden, single-sample 2D Depth32Float_Stencil8 views under registry
lock. Images, unresolved bindings, known same-base aliases, and unknown cases
retain the old boundary. Original attachment-store check and queue barriers stay
on. Mid-pass depth clears explicitly invalidate this candidate until a new pass.
CPU policy exhaustively tests 64 Boolean combinations; guard checks added.
All 14 ITT tests pass and runtime builds. GPU fixture is only one prerequisite;
full runtime equivalence and performance benefit remain unproven.

Started isolated test PID83228, `logs/depth-sample-candidate`, with copies at
`analysis/depth-sample-test-{profile,cache}` and the opt-in switch. No intrusive
per-encoder GPU timing enabled, no automated movement. Matching shared-memory
service unchanged; formal App/saves untouched. F9 capture remains available.
Stop automation on user takeover. No commit or push.


## User-approved workspace commit and GitLab push

User reports “现在稳定很多了” and explicitly authorizes committing ALL workspace
files and pushing to GitLab, including the previously staged Dungeons/build changes.
This supersedes the earlier no-commit restriction but is not an instruction to
replace the formal App or merge test saves. No game session is terminated.
The positive report is not a controlled same-scene FPS measurement or proof of
sustained 120 FPS. Keep the tested opt-in depth-sampling policy unchanged.

Resolved the historical repository-prebuilt blocker: installed the exact tested
metadata-aware service into ITT's repository prebuilt, SHA-256
`a835f5a868f08e5cb1d84ae2d0a2758d039187e2001f0f6bcf20e7901e03cf62`, with matching
digest and provenance notes. The older “prebuilt remains old” sections above are
historical investigation state, not the current packaging state. No formal App
installation performed. For the tested performance behavior, explicitly set
`ITT_DEPTH_SAMPLE_CONTINUE=1`; default rendering behavior remains conservative.

Pre-commit regression results: ITT 14 pass; Dungeons 36 discovered, 29 pass and
7 optional tests skipped; sparse identification 2 pass; container fixtures 16
pass; DLC import fixtures 19 pass. Initial system-Python runs could not import
`cryptography`; reran those suites with the existing external build virtualenv,
without installing dependencies or changing sources. No gameplay/GPU probe was
started during these checks. Existing staged MCD files have three trailing-blank-
line whitespace warnings; preserved frozen source instead of unrelated cleanup.


## Formal App update authorized and installed

User explicitly requested “更新正式app”. With no game runtime running, backed up
the replaced formal App components under external
`analysis/release-ee5ca2b-20261029-175535` (including old signature/Info.plist),
then installed the tested runtime, matching shared-memory translator and 653
validated source-hash-named Metal libraries. The launcher now defaults
`ITT_DEPTH_SAMPLE_CONTINUE=1` and `TOTK_ASYNC_SHADERS=0`, preserving explicit
inherited overrides. The raw runtime's conservative default remains unchanged.

15 ITT regression tests pass, including the new launcher-policy guard. Rebuilt
launcher and ad-hoc signed the App; deep/strict codesign verification passes.
Signing changes launcher signature bytes, so comparison used signature-stripped
backup copies and confirmed identical executable content. Runtime and translator
match their build/repository sources byte-for-byte; all 653 library digests match.
Runtime SHA-256: `9580f97568f0280fb44f0aba6c2c9a96cc7af70364b0eb6a0b925404b400b49b`.
Service SHA-256: `a835f5a868f08e5cb1d84ae2d0a2758d039187e2001f0f6bcf20e7901e03cf62`.

Game-data inode/size/mtime and external RomFS symlink are unchanged. Recorded
formal save/profile files retain their sizes/mtimes; no test saves/cache were
merged into them. No other App updated; no game terminated or automatically
launched. This is an installation/integrity check, not another gameplay or
120-FPS regression measurement. Details in the external backup's `installed.json`.

## Dr. Hakim cutscene candidate (not released)

The formal session preserved externally in `logs/book-stall-investigation-20261029-180553`
has 0.5–2.7 s frames dominated by library/pipeline readiness (about 88–94%),
not GPU execution. Ordinary logs do not identify the book's exact per-frame keys.
No evidence yet proves repeated eviction of the same shader.

Found a scheduling coupling: `TOTK_ASYNC_SHADERS=0` disables shader lookahead as
well as skipped draws; program-set prefetch prepares only already known variants.
Opt-in `ITT_SHADER_LOOKAHEAD=1` decouples preparation from draw omission. Scans
queue urgent exact translation jobs and compile their libraries through the existing
ready hook. Both actual graphics and compute consumers resolve missing stages
synchronously when async skipping is off. The scan retains command/time limits
and defaults to a 4 ms per-frame budget. Shader identity, source, reflection,
compute shared bytes and pipeline descriptors are unchanged. Required shader
lookup/lock/background waits >=20 ms are logged under `TOTK_SURVEY_SHADER_LOG`.
The formal launcher does not enable the candidate.

18 ITT CPU tests pass, including exhaustive preparation policy and an actual
`totk_rt_variant` fixture with fake workers proving parallel distinct jobs,
single-flight requests, blocking required consumption and subsequent reuse.
Native runtime builds. This does not prove cutscene equivalence or stall removal.

User requested current formal saves for reproduction. Copied the formal profile
and verified all three files by SHA-256; formal profile remained unchanged after
startup. Baseline profile/cache copies retained externally. Candidate PID 4516
started with no automated input; controller takeover was detected, so it is left
running. Logs confirm lookahead on and async skipping off.

Test setup limitation discovered after takeover: `TOTK_SHADER_CACHE` accepts the
chain directory itself, but the helper pointed to its copied parent. Thus this
session uses a newly populated test cache beside the copied chain directory,
not the intended warmed cache. No formal cache was altered. Do NOT treat this
session as matched-cache A/B or claim a measured speedup. Do not modify its live
cache or restart the user's session. The external helper's path is corrected for
a future fresh isolated launch. This run remains useful for cold-miss diagnostics
and user visual checks; replay with matching cache copies is still required.

### Second candidate: prepare the current draw before scanning ahead

First-run feedback: the user saw Hakim's appearance rather than missing its
opening during a freeze, but the overlay fell from 120 to about 40 FPS. This is
partial user-observed improvement, not a controlled measurement. The completed
log still contains earlier multi-second stalls and late 21–38 ms frames with
6–7 ms GPU execution and some 11–16 ms synchronous translations. No exact scene
marker associates these late frames conclusively with Hakim.

Lookahead previously started after the current command. The second candidate
queues the current new program's stages urgently before scanning later commands;
non-draw/non-scan program consumers retain their previous synchronous behavior.
`itt_prog_prepare` also queues exact current bindings for missing stages and for
both graphics stages needing a binding refit before either is consumed. It does
not change active shaders, signatures, alternative maps or missing bits. Existing
required resolution waits and validates dependencies normally. No shader identity
or reflection change, no draw omission. Parallel preparation cannot guarantee a
zero-stall first encounter: work first discovered at its draw can still take time.

19 ITT tests pass, including production helper tests for unchanged active state,
cached/pending stages, unchanged bindings, graphics/compute and opt-out. Runtime
build succeeds. Test PID 10583 uses fresh verified formal profile copies and the
correct copied chain directory. Startup log confirms 1043 shaders, 906 render
and 140 compute pipelines warmed in 609.9 ms; zero manifest shaders missing.
`ITT_SHADER_LOOKAHEAD=1`, `TOTK_ASYNC_SHADERS=0`. Controller takeover detected;
no automated input sent and session left running. Logs and immutable starting
copies are under external `book-lookahead-v2` paths. Differences from the first
test include cache correctness, so do not attribute all improvement to scheduling.
Formal App and formal saves remain unchanged; no release/commit/push yet.

### User acceptance and formal release policy

User reports Hakim's appearance is now essentially smooth, with three save reloads
showing essentially no FPS drops, and explicitly authorizes committing/pushing all
workspace changes and updating the formal App. This validates the tested scene,
not a guarantee that every unseen shader is stall-free. The corrected-cache test
session exited gracefully. Its runtime SHA-256 is
`c03942398737ef86ae8eb838d0d8e916068770905a0a3eaa96cfc0a86ebdc560`.

Formal launcher now defaults `ITT_SHADER_LOOKAHEAD=1`, retains
`ITT_DEPTH_SAMPLE_CONTINUE=1` and `TOTK_ASYNC_SHADERS=0`, and honors explicit
inherited environment overrides. Release uses the exact user-tested runtime;
subsequent runtime-source changes are comments only. Existing translator and 653
source-exact offline libraries are retained. No test saves or caches are merged.
No game/GPU probe is launched for packaging while the user's Zelda session runs.
