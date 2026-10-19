# Bound speculative pipeline work on the submission thread

## Problem

On a pipeline cache miss, `pso_get` scans later commands before compiling the
pipeline the current draw actually needs. That scan used `prog_get`, which can
translate and compile a future program and select its binding variant. All of
that work runs on the submission thread. A scene introducing many programs can
therefore delay the current draw with compilation for later draws, before any
predicted pipelines have even been dispatched.

The pending-pipeline table also retained completed predictions which were never
consumed. After enough guesses, its 384 slots stopped admitting new work.

## Change

- Resolve prediction stages only from an already cached program and the exact
  binding signature at the scanned command. If either stage is unavailable,
  skip that prediction; the actual draw still resolves and compiles normally.
- Leave the program's active shaders and signatures unchanged. Update the
  temporary render-target state when scanning a pass so fragment-output
  signatures describe the future pass, then restore the active state.
- Stop after at most 8,192 commands or 32 primary candidates; check a 2 ms budget
  between commands. This is a best-effort scan budget, not a hard bound on the
  entire function, descriptor construction or driver calls.
- Dispatch primary candidates as they are found, instead of waiting for the
  complete scan. Dispatch alternate depth/stencil guesses afterward.
- Reclaim unused completed predictions once more than two submissions old.
  Never reuse an unfinished slot: its worker still publishes through that slot.
  The existing mutex protects both completion and reclamation.

This does not skip actual draws, change frame pacing or resolution, introduce a
new shader queue, or eliminate compilation of genuinely new shaders. Existing
`TOTK_PSO_PREDICT=0` still disables prediction for diagnosis. It is independent
of the optional render-skipping approach in PR #4 and startup warm-up in PR #13,
although the pending-table code overlaps PR #4 and needs coordination if both
are merged.

## Evidence and limits

A local Apple M3 / 24 GiB / macOS 27.2, game 1.0.0 session recorded 32 relative
long-frame entries above one second, all accompanied by library or pipeline
creation. The worst frame was 10,384.9 ms: submission plus presentation took
10,378.8 ms, while GPU execution was 17.5 ms. Nested compilation timers must not
be summed as independent wall time. These observations establish the compilation
bottleneck; they do not attribute every millisecond to this predictor.

The deployed local candidate also included opportunistic shader prefetching and
its worker-process signal-mask handling. The player reported that severe stalls
were substantially reduced; a short loaded-scene check also passed. Those are
observations of the broader candidate, not an isolated benchmark for this PR.
Only the predictor correction and pending-slot reclamation are submitted here.
No same-route, same-cache A/B or full-session 1% low claim is made. Remaining
first-use compilation and sustained frame-rate optimization need separate work.

Raw logs and build products stay outside the source tree. The local diagnostic
archive is under `~/Library/Application Support/MacNativeGames/TOTK/tests/`:
`stutter-20261019-123233`, `stutter-fix-20261019`, and
`pr-bounded-pipeline-prediction-20261019`. No game files, saves, keys, or compiled
shader binaries are included in this change.

## Focused regression check

The production `probe-gfx` CPU probe now also checks that:

1. An unresolved future program with no shader bytes is skipped without
   translation, compilation or changes to the active bindings.
2. An already cached alternate binding variant can be inspected without
   selecting it as the program's active variant; a missing variant is skipped.
3. Old completed pending slots are reclaimed, recent completions are retained,
   and even very old unfinished workers retain their slots until completion.

Existing scissor and conditional-counter preservation checks remain in place.
The probe output names these checks explicitly. It does not create a Metal
device or issue GPU commands; it maps existing local modules for the runtime's
standard probe initialization and does not boot the game or modify its profile.

After building the runtime in an external work directory, run:

```sh
build/totk-native rewritten/v0 probe-gfx
```

Version 1.0.0 is the locally checked game version. Other supported versions and
an isolated playable build of this narrower PR have not been gameplay-validated.

Local validation of this isolated branch on 2026-10-19:

- Native runtime build/link passed. Unchanged native graphics objects were
  reused; `nvn_mtl.m`, the base `hle_nvn.c` and base `totk_shader_rt.m` were
  rebuilt to exclude the broader local prefetch changes.
- The resulting `probe-gfx` passed, including the new unknown-program,
  alternate-variant and pending-slot checks, with no GPU drawing.
- All 38 repository source tests passed using the existing Python 3.14 build
  environment and its pinned dependencies.
- `git diff --check` passed. The renderer still emits 58 compiler warnings;
  this is not a warning-free build.

The installed player App and its ongoing session were left untouched.
