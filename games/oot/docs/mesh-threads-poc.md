# PoC: mesh stage at 64/128 threads per threadgroup on Apple silicon (2026-11-26)

Branch `oot-mesh-simd-poc`. Question: the offline meshbench measured that running a mesh stage
at more threads per threadgroup (same output allocation) is the win — on an M4: 32 -> 128 threads
was −42 % (docs/fps-60-90.md); we re-measured on an M2 with the real fed302d7 structure — and the
real shader crashes above 32 threads. Details and numbers below.

## 1. Microbenchmark, reproduced and committed (`make -C games/oot WORK=… mesh-simd-split-bench`)

`games/oot/tests/mesh_simd_split_bench.{m,metal}`: 1117 threadgroups (fed302d7's grid), 128 vertices
x 10 float4 exported per group, a ~13 KB device scratch slot taken/released with atomics, and 80 raw
word reads per logical lane through an 8-word range cache (the port's raw-read path; the real shader
has 61 4-wide + 19 1-wide). Variants t32/t64/t128 (threads per group) and s64/s128 (reads staged per
SIMD group through threadgroup memory). Median of 16 submits, Apple M2:

| variant | threads | median ms | vs t32 |
|---|---|---|---|
| t32 (as shipped) | 32 | 49.9-51.9 | — |
| t64 | 64 | 26.8-27.3 | −46 % |
| t128 | 128 | 14.4-16.6 | **−69..−71 %** |
| g32 / g64 / g128 (sgid-only body, the real guest shape) | 32/64/128 | 41.7 / 36.4 / 42.3 | ~none (g128 worse) |
| s64 / s128 (read split) | 64 / 128 | 26.1-27.6 / 16.7-16.9 | no better than t64/t128 |

So on the M2 the win is the **thread count per threadgroup**, not splitting one thread's read chain
across SIMD groups (the split adds nothing once the group is bigger).

## 2. Translator PoC: `TOTK_MESH_THREADS_SCALE` in `tools/totk_xlate/MeshStage.cs`

Off unless the env is set. It scales the mesh stage's local size (`job["local"]`, every wrapper
stride, and the reflection's `threads`/`threadgroup_size`) by the factor. Verified faithful: with
scale unset, a JIT build of the translator reproduces the packaged seed's MSL for fed302d7
byte-for-byte (148,784 bytes identical); with scale 4 the MSL compiles and declares 128 threads.

## 3. Runtime PoC: `OOT_MESH_TRANSLATION=3` (kind `mesh_threads`)

`nvn_mesh_translation.inc` / `nvn_mesh_tasks.inc`: mode 3 loads an external candidate whose
reflection may differ only in `threadgroup_size` / `mesh_stage.threads`, and uses the candidate's
thread count for the mesh pipeline and the draw. `probe-gpu-mesh` passes with the patch in default
mode (7,920 checks); full `unittest` 2398 OK; oot and totk runtimes build.

## 4. In-game result on the M2: the real shader crashes above 32 threads

fed302d7 (foliage) re-translated at 64 and at 128 threads, loaded via mode 3, run in Kokiri Forest
(720p + dynamic resolution): both crash with SIGSEGV at the same frame (660), i.e. when the scaled
mesh pipeline first draws. Crash data (mode-3 variant, 64 threads): `signal=0xb fault=0x1
esr=0x92000006`, faulting PC in libsystem called from guest `main+0x1c4280` — a guest-side bad
pointer deref, i.e. the 64-lane run computes a wrong index (the code assumes a 32-lane workgroup).
The local size cannot simply be raised — the benchmark's model does not transfer as-is.

## Root cause of the crash (found 2026-11-26) and the real W1 design

The guest mesh body uses **only** `gl_SubgroupInvocationID` (0..31 per SIMD group) as its lane id and
computes its per-lane raw address as `_77 = (instance << 5) + sgid` (the `<<5` is the hardcoded
32-lane stride). At 64/128 physical threads there are 2/4 SIMD groups with the *same* sgid values, so
they run the identical body concurrently and race on the stage memory — that is the SIGSEGV. The
body has no `thread_position_in_threadgroup`-strided loops to widen (the bench body did, which is why
its t64/t128 variants won there).

Metal limits on the M2 (measured with the bench): a mesh threadgroup may export at most **256
vertices** and **32 KB of total mesh size** — with 10 float4/vertex that is one 128-vertex instance
per threadgroup. So multi-instance-per-threadgroup (the other way to use extra threads) is impossible
here; the only route to 64/128 threads is **flattening the lane id inside one instance**, which means
patching the body's lane-stride constant (`<<5` -> `<<6`/`<<7`) and keeping subgroup masks per-32-lane
in the translator's IR — a per-shader change that needs `probe-gpu-mesh` + screenshot equality against
the 32-thread build as acceptance. The crash repro above (variant at 64/128, SIGSEGV at frame 660) is
the acceptance test: any candidate must render fed302d7's foliage identically to the 32-thread build
*and* not crash.

Bench limits are now printed by failing compiles on any Mac: 512 verts/group is refused (max 256),
and 256 verts x 10 float4 exceeds the 32 KB mesh-size cap on the M2. The `g*` variants (body lane id
= `thread_index_in_simdgroup` only, like the real guest body) show the real shape gains nothing from
more threads without the IR lane flattening: g32 41.7 / g64 36.4 / g128 42.3 ms vs t128 16.6 ms.

## Update (2026-11-26, later): lane-flattening pass implemented in the translator, variant loads but still faults at 64 threads

`MeshStage.cs` gained a flattening pass (behind the existing `TOTK_MESH_THREADS_SCALE`): when the scaled
thread count is a power of two > 32, the body's subgroup lane id used for addressing is replaced by the
global thread id (`dm_lid`, passed in as a new `dm_flat` parameter; subgroup *mask* lines keep the
per-32-lane subgroup id), and the wrapper's stage header `mem[0]` is scaled by `threads/32` so one
threadgroup covers that many 32-vertex blocks (the guest grid stays sized for 32, out-of-range groups
no-op via the body's bounds check). All 32-lane strides/decompositions in the body stay at 32, so the
stage layout and export loops are unchanged.

Validated on fed302d7: the variant generates, compiles standalone (`xcrun metal`), its reflection
equals the seed's except `threadgroup_size`/`mesh_stage.threads`, and mode 3 **loads** it
("loaded mode=3"). But running at 64 threads still SIGSEGVs the guest at `main+0x1c4280` (fault=0x1)
at world entry — the raw-pool address path in the body (not just the `_77` vertex id) still assumes 32
lanes. Next step: trace that faulting access (watch/`mscan` on the pool table read) and flatten it too;
the acceptance stays probe-gpu-mesh + pixel-identical foliage vs the 32-thread build.
