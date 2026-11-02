# Sampler caches, pipeline prediction allowance and wait accounting

These changes integrate the sampler-key identity from [PR #483](https://github.com/ly05010419/MacNativeGames/pull/483),
ready-function reuse from [#286](https://github.com/ly05010419/MacNativeGames/pull/286), adaptive prediction allowance from
[#484](https://github.com/ly05010419/MacNativeGames/pull/484) and wait attribution from
[#487](https://github.com/ly05010419/MacNativeGames/pull/487). They affect the shared TOTK/BotW host in `games/totk`.

## Sampler and function caches

`TOTK_BORDER_KEY_CACHE=0` disables the sampler-key cache. With it enabled (the default), a hit requires the same
shader, stage, sampler IDs and sampler-registry generation. Both checking and publishing a cache entry hold the
registry lock. Re-registering the same ID, unregistering it, switching a combined texture/sampler handle or changing
the stage takes the original key-building path. The cached result includes colours, axes, argument IDs and complete
sampler parameters. `TOTK_BORDER_KEY_CHECK=1` recomputes and compares all of these on every hit.

The larger cache is allocated only when a shader first uses an emulated border. Shaders that never use one do not
allocate it, including on iOS. It retains strings and scalar sampler data, with no textures or buffers. The original
filtered-border shader source and recipe format are unchanged.

`TOTK_SHADER_FUNCTION_CACHE=0` disables the ready-function cache. With it enabled, four recent function results per
shader avoid rebuilding varying/border lookup strings. The peer shader, key and function are retained and compared by
object identity, with the same locks as the original dictionaries. A pending `nil` result is never cached; the next draw
retries it. Warm-up still uses the original dictionaries.

## Prediction allowance

`TOTK_PSO_SCAN_ADAPT=0` keeps the fixed frame allowance. With adaptation enabled (the default), scans may spend up to
the greater of the fixed allowance and time already spent building pipelines synchronously in that frame. Scans,
waiting for workers and background compilation never earn more allowance. The existing per-scan command/key/time
limits and rescan count remain in force. No draw is skipped by this change.

The frame boundary resets both spending and earned allowance. `nvn_scan_budget.h` contains the accounting shared by
production and the CPU regression. Its accumulation saturates rather than wrapping.

## Wait attribution

Set `DM_NVN_TSV` to a file path to collect the added frame rows:

| Rows | Meaning |
|---|---|
| `@xl_sync_ms`, `@xl_wait_ms` | Translating synchronously, or waiting for a translation on another worker |
| `@plib_wait_ms`, `@pso_wait_ms` | Waiting for a predicted Metal library or pipeline |
| `@lib_ms`, `@pso_ms` | Library/pipeline creation accounting |
| `@sp_scan_ms`, `@pp_scan_ms` | Shader/pipeline prediction scan accounting |
| `@core_wait_ms` | Completed virtual-core waits of the calling/presenting guest thread |
| `@core_wait_all_ms`, `@core_waits` | Completed waits of all guest threads, and their count |

Core waits are completed when the scheduler grants a core. This includes BotW's wake queue, which can grant the core
before its host thread reaches `core_take`. Snapshots hold the same scheduler lock as writers. When replay runs on a
host thread, the calling-thread core wait can be zero; the all-thread rows still describe guest waits. These rows have
overlapping scopes and should not be added together as a frame-time decomposition.

`TOTK_CORE_DIAG=1` also enables collection and prints bounded long-wait owner samples and periodic per-thread tables.
Sampling copies retained Mach ports under the scheduler lock, then releases that lock before suspension, symbol lookup
or logging. Sampled owners are resumed before their addresses are described. Tables copy their counters under the lock
and format them after releasing it. Scheduling policy, priorities and preemption rules are unchanged.

With neither switch enabled, the added core/translation collection performs no additional timing calls, counter updates
or snapshot locks. The original renderer timing used by existing diagnostics is unchanged.

## Checks and performance interpretation

Run `python -m unittest discover -s tests`. The focused checks are `test_sampler_caches.py` and
`test_performance_batch.py`: they exercise registry replacement, cache misses/hits, concurrent publication, lazy
allocation and retained identities; budget exhaustion/reset/control; actual scheduler contention and wake-queue
grants, enabled/disabled counters, disabled read cost and native owner sampling.

Run `make probe-gpu-sampler-border` with `TOTK_BORDER_KEY_CHECK=1` for production vertex/fragment variants, address/filter
changes, recipe reuse and GPU pixel readback. `probe-gfx` checks prediction does not mutate active command state.

For gameplay measurements use the project's long-frame recording workflow with separate save/cache copies, the same
route and picture settings. Compare one switch at a time, keep profiling conditions equal, and report pipeline/scan
work separately from frame time. A synthetic repeated-key lookup improvement is not evidence of an in-game FPS gain.
