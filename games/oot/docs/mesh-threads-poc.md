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
| t128 | 128 | 14.4-14.6 | **−71 %** |
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

## What a real fix needs

Restructure the mesh stage so one threadgroup runs several instances at 64/128 threads with a
correct per-lane mapping (the translator's `MeshStage.Wrap` plus the body's lane assumptions), then
re-validate with `probe-gpu-mesh`, the mesh translation modes, and in-game screenshots against the
32-thread output. The crash repro above (variant at 64/128, SIGSEGV at frame 660) is the acceptance
test: any candidate must render fed302d7's foliage identically to the 32-thread build *and* not crash.
