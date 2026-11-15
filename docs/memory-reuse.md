# Shared graphics memory reuse

The shared NVN backend used by TOTK, BOTW, Bananza, Pokopia and Ocarina of Time
has independent switches for the following allocation policies. They do not
change texture resolution, mip levels, rendering commands or effects.

| Policy | Startup environment | Live command | Default |
| --- | --- | --- | --- |
| Empty render-target heaps | `TOTK_HEAP_REUSE=1` | `flag heap_reuse 1` | Off |
| Descriptor tables and synchronous readback buffers | `TOTK_BUFFER_REUSE=1` | `flag buffer_reuse 1` | Off |
| Compact recorder scratch | `TOTK_RECORD_COMPACT=1` | `flag record_compact 1` | Off |

These policies are opt-in while game and device coverage is collected. Setting
the live command to `0` disables that policy. Existing in-flight allocations
retain their original retirement fences.

Heap reuse starts only after the final texture and view have been destroyed.
It preserves existing live storage aliasing. An independent GPU-only,
compressible target can also use its own placement heap. The smallest eligible
empty heap is selected within one eighth of the required size; alias heaps
require an exact size. Waiting heaps are bounded by 64 entries and 256 MiB,
and expire after three seconds. Residency removal is committed at a safe
submission boundary or after replay and GPU work become idle.

Descriptor buffers use exact 64-KiB size classes and completed submission
fences, with a 64-entry / 16-MiB cache. Readbacks use 4-KiB size classes and
return only after GPU completion and CPU copying. Their cache is limited to
16 buffers / 16 MiB. Both budgets count Metal's actual allocation sizes.
New descriptor contents and unused padding are initialized before publication.

Compression applies to the recorder's reusable scratch. Published command
handles keep the existing canonical, patchable representation and hardware
command-memory accounting. Finalization expands draws with one exact output
allocation. It does not reduce the persistent canonical command copies.

Sampling views belong to the backing storage entry or uploaded texture package
and include format, type, mip range, array slices and swizzle in their key.
Replacement and eviction retire the package's views together with its old
backing. Commit feedback handlers and synchronous texture readbacks drain local
autorelease pools. Idle retirement retries a busy owner lock and commits pending
residency changes without requiring another GPU submission. The optional
`TOTK_ASYNC_REPLAY=1` mode retains submission-driven collection because it uses
a separate replay queue.

Pipeline archives already use the scoped public `NSData` mapping hook in
`nvn_archive_map.h`; no private archive API is required.

Native regression tests are `test_heap_cache.py`, `test_buffer_cache.py`,
`test_record_compact.py`, `test_texture_view_cache.py`,
`test_feedback_lifetime.py`, `test_idle_retire.py` and `test_archive_map.py`.
The Metal tests use the shared game slot. Allocation and lifetime probes do
not establish whole-game memory savings, frame-rate gains or long-session
stability. Other independently maintained game backends need separate ports.

Real-game checks use quick/test startup through `tools/game_test_startup.py`.
Before `GameSlot.popen`, it checks the runtime's compiled `MNG_SHADER_CHAIN`
marker, the seed's identity and files, the actual translator, prepared library
coverage and the title pipeline list. Mixed versions and missing preparation
fail before a process starts. `StartupWatch` requests diagnosis after 30 seconds
without startup progress; audio/rumble messages do not extend that deadline.
Seed import and warm-up without a presented frame are not a successful launch.
See `tests/test_game_test_startup.py` for the rejection and no-launch controls.
The caller must explicitly enable `TOTK_GPU_ALLOW_SUBMIT=1`; encoding-only
execution (`TOTK_GPU_NOCOMMIT=1`) is rejected before launch and on its runtime
warning. Frame counters alone cannot establish that GPU work ran or a picture
was presented. Inspect the captured game scene before counting visual checks.

Ocarina of Time's matrix helper keeps its SIMD matrix operations and barriers
in a callable `noinline` function. Expanding hundreds of forced-inline calls
made Apple's pipeline optimizer stall before the title. The real-GPU regression
`tests/test_oot_hmma_calls.py` compares single and chained calls bit for bit
against the inline body and checks an independent CPU matrix reference, with
an intentionally corrupted-output control.

Design references: seedds/nxnative commits
[`7f502220`](https://github.com/seedds/nxnative/commit/7f50222003e4270e3655d4f798e85744d390edf0),
[`87e258bb`](https://github.com/seedds/nxnative/commit/87e258bbda9655c4fb9e712161ce4f0f9fdb955c),
[`c87bb116`](https://github.com/seedds/nxnative/commit/c87bb116a043d87c38f31165ca0ca189df6f7834),
[`1d377206`](https://github.com/seedds/nxnative/commit/1d377206844e1970a639d91a5789c0be8e509b49),
[`6121fd75`](https://github.com/seedds/nxnative/commit/6121fd75312cb4d1af404a01921d437741fea91e),
and [`f02d0474`](https://github.com/seedds/nxnative/commit/f02d0474cf1ed1de32ccee1313e1448d4f7da401).
See `LICENSE.nxnative` for their MIT license.
