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

### W2. Render distance for OoT — blocked and reverted (2026-11-26)
The camera far plane (parameter struct +0x138, loaded every frame at `main+0x4c52ec` and stored to
the projection at `main+0x4c52f4`) was going to get a veneer like the aspect one. Measured deadlock:
**any** `b` replacing the instruction at `main+0x4c52ec` (or at the store `main+0x4c52f4`) hangs the
guest at world entry (~frame 680), even with a semantically identical no-op stub (original load +
branch back), at stub offsets +372, +400 and +2000 — while builds without the patch (main, W5-only)
and the aspect veneer at `main+0x4c5334` in the same loop run fine. So the game depends on that exact
code (it likely rewrites or hashes the region at world entry). The W2 code is reverted on this branch.
Next step: find the site that *writes* the camera struct's +0x138 (scale there, once, instead of the
hot load), or RE what touches `main+0x4c52ec` at world entry. Note the earlier A/B at 25/50/100 % was
run with the broken-math build (fadd not fmul — caught in review) and showed no gain in a static forest
view anyway (draws 32 -> 29 only at 25 %); the real win would be looking across open areas. LOD distance
for OoT still needs its own verified sites (BotW's are TOTK offsets).

### W3. Shadow resolution row (opt-in) — blocked on locating the size source (2026-11-26)
~5.3 ms/frame of depth-pass spans (1000x1000 atlas + 128x128 cascades, resolution-independent). Leads found:
the atlas size is read from a config struct's u16 fields at +0x48/+0x4a/+0x4c by the function at
`main+0x244bb8` (thunk calls at +0x870/0x878/0x880 = SetWidth/SetHeight/SetDepth); the struct arrives as
the function's `x0`, reached via indirect dispatch (no direct `bl`/`adrp` refs found in the scan). The
atlas texture is **re-created ~1.3x per frame** (5,285 registrations in ~3,900 frames), which is why
naive HLE scaling at SetFormat measured *worse* (44.5 vs 50.6 fps — re-upload churn, GPU p50 43 vs 21 ms):
any fix must change the size where the game computes it, once, so viewport/sampling/atlas layout all
follow. Acceptance when unblocked: screenshot diff shows only softer shadows; opt-in row; ~2-3 ms/frame.

### W4. Compute-encoder sharing — mostly shipped; remainder is the conservative analysis (2026-11-26)
`TOTK_DISPATCH_REUSE` (on by default) already shares one compute encoder between dispatches the
dependency analysis (nvn_dispatch_order) clears: measured on the M2 at DRS, reuse off = 48.1 fps vs
on = 50.6 (**+5 %**). Dispatch stats at frame 3900: ~7.8 dispatches/frame, of which ~63 % still open
their own encoder because a binding has no known guest range (textures, images, descriptor pools,
pointers from uniforms). `TOTK_BARRIER_START=0` and the experimental `TOTK_BARRIER_NARROW=2` both
measured neutral while the GPU is saturated, so the remaining win is encoder fixed cost, not
serialization. Extending the range analysis to texture/descriptor-pool writes is the remaining work —
correctness-critical, gated by `probe-gpu-dispatch-chain` (its negative control) + screenshot
equality. Expected: ~1-2 ms/frame more if it lands safely.

### W5. Frame-drop spikes: pre-warm streamed textures on area entry — implemented, opt-in (2026-11-26)
First visit to an area uploads up to 383 textures / 280 MB in one frame (100-680 ms spikes). Implemented on
this branch: `nvnTexturePoolRegisterTexture` queues the slot (`OOT_TEX_PREWARM=1`, off by default), and the
submit thread drains the queue at the start of every submission (`pw_drain` in `nvn_mtl.m`) with a
per-submission byte budget (`OOT_TEX_PREWARM_MB`, default 48, full budget during registration bursts).
Measured on the M2: 27k+ textures / 29 GB pre-uploaded ahead of first use, fps unchanged (50.0 vs 50.6);
the area-entry spike drops a little (681 -> 586 ms) but is not gone — the textures bound on the entry
frame are registered in the same frames, and part of the remaining spike is CPU-side (0 textures, GPU
31 ms, frame 237 ms). Next step for W5: hook the game's area-load state (or drain unlimited during fades)
so the entry frame uploads nothing. Acceptance: long-frame spikes gone on a scripted route; no fps cost.

### W6. CPU headroom — measured, no safe win left on the host side (2026-11-26)
`sample` over 15 s on the M2 (Kokiri, DRS): **90 % of all CPU samples are in guest JIT code**, the host
runtime is ~10 % and mostly *waiting* (`__semwait_signal`, gate) — the busy host work per frame is
JIT translation 0.6 ms + NVN submit/present p50 0.66 ms (~1.3 ms of a 16.8 ms frame). The pipeline at
DRS is **guest-CPU-bound** (2.04 M guest instructions/frame, ~63 k per draw, 32 draws/frame; GPU busy
p50 only 4.4 ms). Argument blocks: 60-70 encodes + 3-4 rebinds per frame; `TOTK_AB_SAME` A/B measured
neutral (43.98 vs 43.5 fps) — the memcmp is not a cost and same-bytes hits are rare (per-draw uniforms
always differ). Conclusion: the M2 wall at DRS is guest execution + GPU, not host code; finer arg
invalidation or fewer per-draw lookups would save < 5 % host, inside noise. The lever that actually
reduces guest CPU per frame is fewer draws (W2 far plane) and less guest-side work; 120 Hz Macs are
limited by the same guest CPU, so W6 is closed as measured-no-op rather than shipped blind.

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
