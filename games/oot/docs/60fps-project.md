# Ocarina of Time: project plan for constant 60 FPS on the baseline Mac (M2)

Draft PR — the umbrella for the deep performance work. Baseline reference: Apple M2 (fanless,
10-core GPU), macOS 27, internal 2560x1664 @ 60 Hz, build from `786ddc00`. All numbers below are
measured on that machine in Kokiri Forest (newest save, standing still) with metalperftrace,
`TOTK_BENCH_FRAMES`, and the in-game pass/present counters; method: 40 s A/B windows, 2 runs each,
warm cache, host otherwise idle.

## Where we are (measured 2026-11-26)

| Config | game fps | displayed FPS |
|---|---|---|
| 720p native, fixed scale | 41 (24.2 ms) | 41 |
| 720p native + game DRS | 49.3-51.5 (19.6 ms) | 51 |
| 1080p native, fixed / +DRS | 34.9 / 51.5 | 34.9 / 51.5 |
| any + DRS + MetalFX interpolation | ~30-40 real | **60.0 steady** (present interval p50/p95 16.67 ms) |

The frame is **GPU-bound** (on-GPU 19.3-27.9 ms, layer ~100 % active) and **draw/pass/compute-bound,
not pixel-bound**: fixed internal scales 1.0 / 0.75 / 0.6 all measure ~41 fps at 720p. Per frame at
DRS (~683x384 internal): ~1,210 draws, 114 render passes, ~31 compute passes (10.2 ms of encoder
spans, ~4.5 ms of kernels), shadow passes (1000x1000 + 128x128) ~5.3 ms of spans. Render kernels
~15 ms vs compute ~4.5 ms — the wall is the geometry/mesh/fragment side. CPU is ~1.5 cores of 10;
the submit path tracks the frame (replay ~8.25 ms/frame CPU, 2,117 arg blocks/frame at 4 % reuse).

Already shipped in PR #2388 (merge first): dynamic resolution by default on base M1/M2, the
settings row, the pend-span scan skip. PoC in PR #2391: the mesh-threads benchmark + translator
flag + runtime mode 3, with the crash-repro.

## Goal

Constant **60 FPS real** (no interpolation) in Kokiri Forest and the other biomes on the M2, i.e.
<= 16.7 ms GPU/frame, without changing the picture on stronger Macs (any quality trade-off behind
an opt-in row, off by default on M3+; on M1/M2 it may default on, like the DRS row).

## Workstreams, ranked by measured expected gain

### W1. Mesh stage: use the idle threads — root cause found, design set (2026-11-26)
The microbenchmark (#2391) shows 32 -> 64 -> 128 threads per threadgroup is 49.9 -> 26.8 ->
**14.4 ms (-71 %)** on the M2 *for a body with threadgroup-strided loops*. The real guest body does
not have them: it uses only `gl_SubgroupInvocationID` (0..31 per SIMD group) and a hardcoded
`<<5` 32-lane stride for its per-lane raw address — so at 64/128 threads the SIMD groups duplicate
the same lane ids and race on the stage memory (the measured SIGSEGV, guest `main+0x1c4280`).
Metal limits on the M2 (bench-verified): max **256 vertices** and **32 KB mesh size** per threadgroup,
so with 10 float4/vertex only one 128-vertex instance per threadgroup fits — multi-instance per
group is out. The only route is flattening the lane id inside the instance: in the translator's IR,
patch the lane-stride constant (`<<5` -> `<<6`/`<<7`) and keep subgroup masks per-32-lane, behind
`TOTK_MESH_THREADS_SCALE`, per shader. The bench's `g*` variants (sgid-only body = the real shape)
prove the point on the M2: g32/g64/g128 = 53.5/45.3/45.6 ms (no gain) vs t128 13.6 ms (-73 %) for a
lid-strided body — so the win is real but only reachable through the IR patch. Acceptance per shader: `probe-gpu-mesh` passes, the mode-3
candidate renders fed302d7's foliage **pixel-identical** to the 32-thread build, no crash; then a
new chain (v29+) + full seed + archive rebuild, A/B measured. Expected: several ms/frame on every
Mac, most on the weak ones. This is the long pole: per-shader IR patch + full GPU validation.

### W2. Fewer draws: render distance / LOD rows for OoT (BotW already has them)
BotW's `graphics-lod.txt` / `graphics-far.txt` rows cut draws in heavy scenes. For OoT the knobs
live in the game's config (data fields, not immediates — the 1000x1000 shadow size is read as
`ldrh` from a struct, so sites need real RE, or HLE-level scaling). Acceptance: screenshot equality
at 100 %, A/B per biome. Expected: scene-dependent, big in foliage-heavy areas.

### W3. Shadow resolution row (opt-in) — ~5.3 ms/frame of depth passes at 720p
1000x1000 atlas + 128x128 cascades, resolution-independent cost. A proper game-side patch (verified
site) or a smarter HLE scaling (the naive HLE scale measured *worse* — texture re-uploads — so the
patch must avoid reallocating per frame). Acceptance: screenshot diff shows only softer shadows;
opt-in row, default off on M3+, on M1/M2 only if the user wants. Expected: ~2-3 ms/frame on M2.

### W4. Compute-encoder merging — ~10.2 ms of spans vs ~4.5 ms kernels
Merge adjacent compute encoders when the dependency analysis (nvn_dispatch_order) says no barrier
is needed between them. `TOTK_BARRIER_START=0` measured neutral while the GPU is saturated, so the
win is encoder fixed cost, not serialization. Acceptance: `probe-gpu-dispatch-chain`, `probe-gpu-*`
barrier probes, screenshot equality. Expected: ~1-3 ms/frame when it lands.

### W5. Frame-drop spikes: pre-warm streamed textures on area entry
First visit to an area uploads up to 383 textures / 280 MB in one frame (100-580 ms spikes). Pre-load
the next area's texture set during the load screen / fade. Acceptance: long-frame log spikes gone
on a scripted route; no fps cost elsewhere. Expected: removes the 100-580 ms spikes entirely.

### W6. CPU headroom for high-refresh Macs (M3/M4 at 120 Hz)
Replay 8.25 ms/frame CPU, arg-block reuse 4 %. More reuse (finer invalidation keys), fewer per-draw
lookups. Acceptance: replay CPU/frame measured down, no new PSO/UBO probe failures. Helps 120 Hz
Macs hold 60-120 more easily; not the M2 wall.

## Order of work

W1 (biggest, hardest) -> W5 (kills the spikes, independent) -> W3 -> W4 -> W2 -> W6.
Each workstream is its own commit series on this branch, merged to the draft as it lands; the draft
stays open until the M2 holds 60 real in Kokiri Forest **and** a scripted sweep of the other biomes.

## Testing rules (from CONTRIBUTING + this project's practice)

Every perf change: 8 x 30 s A/B/B/A/B/A/A/B windows or 2 x 40 s interleaved runs, full frame
distribution p50/p95/p99/max kept, GPU probes for the touched path (`probe-gpu-*`), screenshot
equality against the default build, no new red log lines, `python -m unittest discover -s tests`
green, oot **and** totk runtimes build (shared `d2r_gfx`), iOS compiles (`game_patches_ns2.c` is
excluded there; shared files are platform-neutral C). Diagnostics off by default, env or input-file
switches only. No game content, ever.

## References

- `games/oot/docs/m2-performance.md` — all the baseline measurements (PR #2388).
- `games/oot/docs/mesh-threads-poc.md` — the W1 PoC numbers and the crash-repro (PR #2391).
- `games/oot/docs/fps-60-90.md` — the 2026-11-07 M4 meshbench history (SIMD split -23 %/-40 %,
  thread-count -42 % at 128 threads on M4).
