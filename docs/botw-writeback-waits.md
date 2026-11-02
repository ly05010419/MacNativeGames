# BOTW write-back core handoff (#552)

[Issue #552](https://github.com/ly05010419/MacNativeGames/issues/552) includes CPU samples with
`mtl_pool_invalidate -> wb_write_list -> pthread_cond_wait`, alongside long frames with little measured GPU work.
The samples establish that the game waits in write-back; they do not establish that this wait causes every long frame.

The shared renderer's mapped-pool write-back waited for translation, the relevant rendering/staging fences, and uploads.
The translation drain already released the caller's virtual CPU core, but reacquired it before returning. The subsequent
GPU/upload wait then held that core while sleeping. Game workers restricted to the same core could not use it, even
though the caller was doing no CPU work. Queue/sync waits already use `NVN_BLOCKING` to avoid this.

`wb_wait_gpu` now uses that same guard around the drain and fence/upload wait. It returns with the caller's core
reacquired, before freshness checks and guest-memory copying. It holds no renderer lock across the handoff. Calls from
host threads and calls inside another blocking guard preserve their existing core ownership.

The guard deliberately ends before write-back marks memory pages busy. Releasing the core during the later per-level
read-back would allow a worker to take it and fault on those busy pages, preventing the writer from reacquiring the
core to finish the copy. That later wait keeps its existing behavior.

Fence selection, staging, CPU overwrite checks, pool lifetime checks, texture contents and resolution are unchanged.
There is no new setting or game-code patch.

## Validation

- `tests/test_writeback_wait.py` compiles the production `wb_wait_gpu` helper and `WB_WAIT_GPU` macro with the real
  scheduler. A worker restricted to the waiting caller's core must run before simulated GPU completion. It checks core
  restoration, nested guards, host-thread calls, both fence-wait modes, and TOTK/BOTW 1.6.0/BOTW 1.9.0 builds.
- Removing the helper's guard makes the regression fail at the simulated GPU wait: the caller still owns its core.
- Build all six Mac runtime variants. Compile the changed renderer for iOS too.
- Run the existing `probe-gpu-writeback` with Metal validation for TOTK and both BOTW builds. The probe covers GPU
  read-back, later CPU writes, neighboring bytes, completion-time writes and retired pools. A standalone host entry
  can call `runtime_gpu_path_probe` without loading game files; this exercises the production renderer, not gameplay.
- Run `python -m unittest discover -s tests` with the pinned dependencies and normal display access. The existing
  settings-window test needs display access; it fails with restricted access on this Mac.

## Gameplay report and further comparison

The reporter rebuilt BOTW 1.9.0 with this fix and reported substantially smoother gameplay ("way better") on the Mac
from #552. This is a qualitative gameplay report; no before/after frame-time capture or specific route was recorded
in the follow-up. It is not a measured percentage improvement or proof that every source of stutter is resolved.

For further comparison, use the same inputs and options, including both DLC packs.
The fix is enabled automatically. Use the same save, route, resolution, frame cap and interpolation setting, let startup
and shader warm-up finish, and capture several runs under equal logging/profiling conditions. Check terrain, grass,
water, Magnesis/Stasis picking, menus and a warp as well as frame pacing.

For a PR attributing a gain to this patch, compare against an unmodified build of base commit `ff63a16`, not just the
older app from the issue: current main also contains other scheduling and graphics changes. Report game-frame p95/p99,
long-frame counts and maximum separately from displayed/interpolated FPS. A passing scheduler regression demonstrates
that workers can use the released core; it is not an in-game FPS or stutter measurement.

The automated checks built all six Mac runtime variants and compiled the changed renderer for iOS; they did not package
every Mac game version or a full iOS app. No iOS device run or quantitative gameplay measurement is claimed.
