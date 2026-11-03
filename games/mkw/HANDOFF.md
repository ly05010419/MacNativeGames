# MKW native port handoff

## Objective and repository

Continue toward a playable native Mario Kart World port. It is not playable. Branch: `feat/mkw`; last committed baseline is `82dc12ae`, a single Kolkka-authored commit pushed to `kolkka/feat/mkw`. Subsequent work is uncommitted; inspect `git status` and preserve it. User previously requested Kolkka identity and a single MKW commit. Do not imply later work is already committed/pushed.

The supplied NSP and `/Volumes/DATA/MKW (Tested Working 1.0) (smallgames.ch)/base/exefs` have matching main/sdk/subsdk0 hashes. Full RomFS equivalence was not checked. `assets/expected-v1.0.0.json` records the verified NSP hashes. No game content belongs in Git.

## Current native state

- Boots and translates SM86 graphics/compute through `tools/ampere_service`, a .NET 10 line-protocol wrapper around nxnative's external batch frontend.
- NVN2 control layout, stage IDs, constant area, compute layout and push constants are implemented. Cache identity is `mkw-sm86-v5-unverified`.
- Native indexed indirect draw, counted multi-draw and compute indirect dispatch use synchronized GPU readback as a conservative fallback. `probe-gpu-indirect` passed 13 checks.
- Mesh calls retain their arguments, then fail explicitly at replay; task/mesh translation and execution are absent. The SDK's DrawMeshTasks is at sdk+0x5e2e80; the first observed arguments are (0,1). Stencil-cull hint at sdk+0x5e9270 takes function/reference/mask and is independent of the real stencil test.
- A 250 MiB unaligned physical pool was failing startup mapping. The new contiguous-backing path in totk_vpool.c aliases original CPU storage only before virtual address/resource publication, keeps coherence, and rejects subsequent remaps. `probe-vpool` passed 39 CPU/GPU checks. Native logs confirm the failure disappeared.
- Guest reservation was reduced to HEAP_OFF + 10 GiB heap cap; startup succeeded after prior reservation failures.

## Latest actual GPU result and next blocker

A bounded actual GPU run after the pool fix reached frame 208, then translation failed on compute opcode 0x59F at offset 0xC0:
The instruction is in the private captured code; do not copy game shader binaries into Git.
Shader key: ab77eaee44cfef889f8d252a6d583cb3132e1c3c8189a2676212dbb5d68030fa.
Logs: `/private/tmp/mkw-alias-gpu.log`. Request/code are in `/private/tmp/mkw-codex-run/build/shader-cache/rt-mkw-sm86-v2-unverified/ab77*`.
This is SURED direct 2D ADD U32, not a tensor instruction. Eglantine's frontend identifies the encoding; nxnative supports 0x593 SUATOM via InstEmitAmpereMem.cs but not 0x59F. A possible next implementation is equivalent non-returning atomic add via SUATOM with RZ destination, preserving descriptor mapping, predicates, scheduling and memory semantics; verify before relying on it.
The request's `nvn2_cs` layout is [1,1,0,56,0]; c0 byte offset 48 maps through AMapC0 to image handle 0 at c2+0x120. The runtime service already passes this layout to the frontend.

A previous 20-second actual GPU run showed a black game area and debug overlay; it did not show verified game imagery. No screenshot was captured before the pool-fix run exited. Do not claim playable graphics or that the black frame is fixed.

## GPU testing authorization and containment

`nogpu()` in nvn_mtl.m defaults to no submission and documents four prior GPU-driver kernel panics. Earlier present counts with the guard enabled were CPU/replay notifications, NOT GPU rendering. Standalone probes set their own GPU permission and are actual GPU tests.
The user explicitly approved short full-game GPU tests after being informed of the crash/restart risk. Keep tests bounded (~20 seconds), clean up the spawned service process group, and do not disable the guard globally. Set TOTK_GPU_ALLOW_SUBMIT=1 only for the test. Tests so far have not kernel-panicked. Partial clears require TOTK_GPU_MIDCLEAR=1; standalone midclear passed 9,216 checks. Full probe-gfx is not green: it fails its inherited copy-mark assumption in default async mode; with async off it previously hung later. Focused push/mesh recording checks run before that failure.

## Local development paths (temporary, verify they still exist)

- `/private/tmp/mkw-codex-run`: isolated run directory, rewritten/extracted/game-data symlinks point to ~/Downloads/MKW_build/mkw; own user data.
- `/private/tmp/mkw-nxnative` pinned source 65501fbbaf8526f7aab0a1edd028135888590f8e; external frontend: dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate; cross: dm/gfx/tools/spirv_cross_bin/spirv-cross.
- `/private/tmp/mkw-ampere-service/ampere_service`: built service; .NET at `/Users/luis/.dotnet`.
- `/private/tmp/mkw-full-corpus`: 4,230 unique archive shaders, private game data.
- `/private/tmp/mkw-bnsh-jobs`, `/private/tmp/mkw-vprs-all`: batch jobs/results; private.
- Reference clones: `/private/tmp/mkw-eglantine`, `/private/tmp/mkw-pokedinner`, `/private/tmp/mkw-eden-android`. GPL references, not copied into MIT runtime. See tools/SOURCE_NOTES.md.

Build runtime: `make -C games/mkw -j4 binary`.
Build service: `DOTNET_CLI_HOME=/private/tmp/mkw-dotnet ~/.dotnet/dotnet build games/mkw/tools/ampere_service/ampere_service.csproj -o /private/tmp/mkw-ampere-service -p:BaseIntermediateOutputPath=/private/tmp/mkw-ampere-obj/`.
Copy runtime into isolated run/build, then from isolated run execute `build/mkw-native rewritten/mkw boot` with:
DOTNET_ROOT=/Users/luis/.dotnet
MKW_DMSH_XLATE=/private/tmp/mkw-nxnative/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate
MKW_AMPERE_SERVICE=/private/tmp/mkw-ampere-service/ampere_service
MKW_SPIRV_CROSS=/private/tmp/mkw-nxnative/dm/gfx/tools/spirv_cross_bin/spirv-cross
TOTK_RENDER_STREAM=0
TOTK_GPU_MIDCLEAR=1
TOTK_GPU_ALLOW_SUBMIT=0 or 1 explicitly
Requires display access outside the sandbox. Use a bounded subprocess with start_new_session=True, then terminate its process group (including translation children).

## Offline evidence and tests

- `tools/extract_bnsh.py` extracts BNSH and SARC/BAHS; 7 structural tests pass in tests/test_bnsh.py with MKW_build/venv Python (zstandard installed).
- Full loose shader set: 1,107 vertex, 2,626 pixel, 472 compute, 25 tagged task/mesh. 4,203 ordinary jobs accepted; 25 task/mesh and 2 unknown graphics rejected.
- Mesa NAK identifies attribute 0x5C as per-primitive shading rate. Service v2 rewrites only scalar static vertex stores to this output into NOPs for full-rate shading. All 1,038 previously failing archive shaders passed the service and Metal's newLibraryWithSource compiler. This is not rendered equivalence. tests/check_vprs_service.py checks the change and byte preservation against private inputs.
- tests/nvn2_shader.c passes synthetic stage/length cases and all 36 initial captures.
- Original 36 startup shaders translate and compile, but that narrow sample does not cover gameplay.

## External emulator comparison

Eglantine build135 displayed title artwork at about 9–12 FPS in a separate test; no race was verified. It is not the native port. Temporary app and a 23 GB game.romfs live under /private/tmp/mkw-eglantine-*; process was stopped/exited. prepare_eglantine.py is only a reproducibility helper, not a substitute for the native objective.

## Latest continuation: SURED candidate implemented

Service Program.cs now lowers the narrowly validated direct 2D ADD.U32 STRONG.GPU SURED (0x59F) to the existing SUATOM (0x593) path with RZ destination and PT result-predicate selector. It preserves descriptor/coordinate/data registers, predicate, instruction size and scheduling. Other forms stay unsupported. The formerly failing shader now translates and compiles with Metal. This is compilation evidence only: GPU atomic results still need a standalone readback test before a full-game run.

Image atomics must use native MSL 3.1 texture atomics. Service.cs detects OpImageTexelPointer in SPIR-V and raises only those modules to MSL 3.1; it rejects the raw-device-pointer emulation. The corrected shader's MSL now uses texture.atomic_fetch_add, with one image binding, rather than the unsupported extra atomic pointer. Writable R32Uint/R32Sint textures created by the native descriptor path request MTLTextureUsageShaderAtomic. Validate views, missing-image fallbacks and GPU readback before trusting gameplay.

The runtime cache identity was bumped to v3, so previous MSL/reflection is not reused. Rebuild runtime after this header change and copy it to the test directory. No full-game GPU test has been performed with this atomic change. The current .NET service binary is /private/tmp/mkw-ampere-service; tests for the exact formerly failing private shader are in /private/tmp/mkw-sured-test.py and outputs in /private/tmp/mkw-sured-test.


## Latest continuation: atomic GPU validation and next failure

`tests/run_atomic_probe.py <service> <spirv-cross>` generates its own SM86 instructions, translates through the real service, compiles the emitted MSL and executes it on Metal. It passed: 64 atomic increments change pixel (0,0) from 10 to 74, while three neighboring pixels remain 20/30/40. No game code is included. Set DOTNET_ROOT and MKW_DMSH_XLATE as above. The generated instructions explicitly disable dependency barriers; omitting scheduling bits defers writes in the frontend and incorrectly tests add-zero.

A subsequent authorized bounded GPU run used v3, passed the former SURED translation failure, then aborted (exit -6) on compute reflection rather than reaching gameplay. No kernel panic was observed. Log: `/private/tmp/mkw-atomic-gpu.log`. New key: `301cffe76e934511d3aa22f43da5212265949ed3d749e2d1cf683757ef57d6ac`, variant `1392ebee2db2`; private job/code/MSL/reflection are under `/private/tmp/mkw-codex-run/build/shader-cache/rt-mkw-sm86-v3-unverified/301cff*`. Layout [1,1,2,64,0], local size [32,1,1]. Reflection rejects `drv_unfillable: ["c0+0x0"]` (vec4 granularity). MSL actually reads c0 byte offset 4, multiplies by 32 and uses its reciprocal to normalize a global invocation index. This suggests dispatch X count at c0+4, but the NVN2 driver header layout has NOT been verified; do not enable it by accepting zero or guessing all four words. Existing Maxwell compute counts at c0+0x200 are filled correctly; the Ampere frontend remaps resource tables but leaves the first 16 bytes unchanged.

SDK image privately decompressed at `/private/tmp/mkw-sdk-image` for analysis. sdk+0x5e3320 records three dispatch arguments into a software command method 0xec8, sdk+0x5e33a0 records the corresponding indirect form (0xed4). Trace queue replay/header setup to establish the first c0 words, or verify with an emulator capture. Native log also reports image slot texture 32142 lacks writable usage; investigate usage promotion/views before claiming atomic runtime binding is correct. Missing-image fallback currently uses RGBA32 integer images and does not support native R32 image atomics. Mesh rendering is still absent.


## Latest continuation: narrow compute-grid candidate (v4)

Service.cs canonicalizes exactly scalar `cp_c0)._m0[0].y` to `cp_c0)._m0[32].x`, the existing replay dispatch-X table at c0+0x200. This is inferred from the failing kernel multiplying c0+4 by its 32-thread local width and normalizing its global X index, NOT independently verified against the console driver. Unknown header words (byte offsets 0,8,12), vector reads and dynamic indexing remain rejected. The private 301cff shader now has no unfillable reads. The end-to-end synthetic GPU test adds the canonical grid count (7) 64 times, yielding 458 from seed 10; a deliberately different header word (99) catches failure to remap. The ordinary atomic readback and unknown-word reflection rejection also pass. No claim of full layout or rendered equivalence.

Reference emulator captures were obtained with EDEN_NATIVE_FRAME_CAPTURE=1 and EDEN_NX2_MME_DUMP=1 during bounded 20-second runs. Private outputs under `/private/tmp/mkw-eglantine-run`: `native-graphics-mme-*.bin`, `native-compute-mme-*.bin`, `mme-calls.bin`, `compute-capture.log`. The graphics and compute engines have separate MME programs. Their captured calls did not include dispatch method 0xec8, so these do not prove the header layout. A diagnostic helper outside the repo (`/private/tmp/mkw-mme-probe.cpp`) includes the GPL reference interpreter for offline analysis; neither reference implementation nor private macro code was copied into the port.

The first v4 bounded native GPU run `/private/tmp/mkw-grid-gpu.log` hit the 20-second bound without an abort, but did NOT reach the 301cff shader (no corresponding v4 cache entry). It is not validation that the game passed that stage. The image-usage warning now logs stage/slot, Metal usage and pixel format, NVN flags/format and view format to diagnose the missing writable binding rather than silently changing resource semantics.


## Latest continuation: Y count and image diagnostic (v5)

The warm v4 run `/private/tmp/mkw-grid-warm-gpu.log` DID pass 301cff and aborted on `c0cc4a3de4887097471f67c9460b955603bea9b5c07f2d62ccca3e9ea9319c88-58bc659fca17` reading c0+8. Its kernel uses this for the global Y dimension, matching the X-count inference. The v5 service now maps scalar c0+4/+8 to canonical X/Y counts. Both private captured shaders pass reflection. Synthetic GPU readback returns 458 for X (seed10 + 64*7), 714 for Y (seed10 + 64*11); scalar byte offsets0 and12 still report unfillable. Full vector/swizzle and dynamic accesses are not mapped. Cache v5 is current; v4 is historical. Unknown-header protection is retained.

Image warning now identifies texture32142: compute stage5 image slot0, Metal usage0x1 (read-only), Metal pixel65, NVN flags0x4 (IMAGE already set), NVN format0x11, no registered format view. Therefore do not fix by setting NVN IMAGE: it is already present. Inspect serial/NvPkg reuse and views. tex_desc already requests ShaderWrite for this flag. `stor_acquire` can promote shared usage through `stor_recreate`; `tex_upgrade_pfv` shows how to preserve GPU content across replacement for shared and NvPkg holders. Any image promotion must also invalidate registry views and preserve encoder ordering. No writable promotion has been implemented yet.


The v5 native GPU run `/private/tmp/mkw-grid-v5-gpu.log` reached the 20-second bound without an abort or observed kernel panic. Both 301cff and c0cc have successful runtime v5 reflection artifacts, and the log continued into hundreds of graphics draws after the earlier compute failures. This is progress past the prior translation/reflection failures, not validation of correct rendered pixels or gameplay. The image warning recurred for texture32164 with the same flags/format/usage. Memory logging showed ~25,864 MiB Metal allocation during the run; investigate resource pressure as well as content correctness. Process group was terminated at the bound.


## Latest continuation: writable image view fixed; mesh reached

The read-only image warning was caused by the sampling swizzle, not missing NVN flags or storage usage. A direct Metal check on RG16Float reproduced backing usage3 -> swizzled RG01 view usage1 -> unswizzled view usage3. `image_fast_rid` now resolves/validates the sampling descriptor, then caches an identity-swizzle image view from the root backing texture with the same pixel format, target, mip range and array layers. Sampling bindings retain their swizzle. Registry invalidation defers release of both views. This needs no storage copy or usage promotion.

`probe-gpu-storage-view` now includes an RG16F image-write regression through the real dispatch/argument-binding path: selected mip1/layer1 receives half-float .25/.5; other mips/layers remain zero; sampling view stays read-only; cached image view is cleared on descriptor replacement. All 15,689 checks passed (`/private/tmp/mkw-image-probe.log`). Runtime build and git diff --check passed.

The next authorized bounded GPU run `/private/tmp/mkw-image-gpu.log` had no writable-image warning, advanced through additional graphics/compute work, and aborted at the explicit OP_MESH_TASKS guard (first0,count1). No kernel panic was observed. There is still no verified game imagery or gameplay. Task/mesh execution is now the immediate observed blocker. A diagnostic at the guard now logs each currently bound NVN2 shader's control stage, control ID, instruction and code-area lengths (rebuild to use it). Runtime/build binary copied into isolated run predates that final diagnostic only; functionality matches.

Mesh starting points: prog_t stores up to6 shader descriptors (not stage-indexed), but NvProg and many replay/resource arrays are indexed over6 ordinary stages. Control stages6(task)/7(mesh) are recognized but rejected in shader prep and ordinary prog_get. Stage-buffer and mesh output translation are absent in the external MIT frontend. The GPL reference has behavioral implementation in src/shader_recompiler/frontend/maxwell/translate/impl/internal_stage_buffer_entry_read.cpp (NX2 pseudo ISBERD/ISBEWR); inspect independently, do not copy GPL code into the MIT port. Existing shader_save in hle_nvn.c is gated by MKW_SHADER_DIR and currently called by ProgramBuilderSetShaders only. First failing mesh program should be captured and correlated with the25 archive task/mesh binaries before choosing the lowering design.


## Latest continuation: PR983 mesh translator integrated offline

User supplied seedds/nxnative plus PR1003, PR983 and PR981. Main HEAD is still65501fbb. PR983 is the directly relevant MIT implementation (task/mesh stages for MKW). It includes the rebased PR981 Ampere fixes. Isolated checkout `/private/tmp/mkw-nxnative-mesh` HEAD `b124b24` with commits5ee98b6(mesh) and7390446(Ampere); old checkout preserved. PR1003 is only Bananza GPU timestamp profile/docs; reviewed, not applied to MKW.

Vendored `tools/dmsh_meshstage.py` from PR983. `prepare_ampere_jobs.py` now accepts task/mesh controls, strips SPH for compute translation, derives layout/payload/thread/output metadata, reserves stage shared memory and rejects own-shared-memory shaders and oversized outputs. `wrap_mesh_stage.py` converts the emitted kernel to object/mesh functions and normalizes JSON component lists to tuples. Service.cs passes shared/layout/metadata to the frontend, runs this wrapper when requested, reflects ts/ms driver tables and records the stage metadata. Set MKW_MESH_STAGE_WRAPPER to its absolute path. Service build `/private/tmp/mkw-ampere-service-mesh` passed. Native host wiring is NOT implemented yet: runtime still uses v5, the old external frontend/service in its launch script, and OP_MESH_TASKS still fails explicitly. Mesh experiments used chain mkw-sm86-v6-mesh-unverified privately; bump native cache when enabling the new frontend.

Evidence: prepare produced4228 jobs (4203 ordinary+25 task/mesh),2unknown rejected. PR frontend batch reported4228 SPIRV outputs. This does not prove ordinary shaders' semantics: unsupported-output notes still need the existing service handling. All25 mesh jobs passed our service with no unfillable driver reads or unknown storage bindings. Generated sources compiled through Metal newLibraryWithSource. Private paths: `/private/tmp/mkw-mesh-pr-jobs/mesh-jobs.json`, `/private/tmp/mkw-mesh-pr-metal`, `/private/tmp/mkw-mesh-service-results`; service replies `/private/tmp/mkw-mesh-service-replies.json`.

Synthetic GPU proof: tests/make_mesh_probe.py generates a task kernel that passes draw-first7 through payload as7/16, and a mesh kernel writes a fullscreen triangle with that value as a varying. tests/mesh_probe.m creates a real object/mesh pipeline, renders16x16 and verifies all256pixels. Passed (`/private/tmp/mkw-mesh-gpu-probe/check`). No game instructions included. It validates the stage wrapper/payload/raster path, not the game's native binding or mesh outputs.

First native mesh program captured from `/private/tmp/mkw-first-mesh-warm.log`:3descriptors, fragment(control stage1) instruction1792 bytes, task(stage6)640 bytes, mesh(stage7)9600bytes; mesh code area9728. All embedded control IDs are ZERO. Existing shader_save keyed only that ID, so its initial capture overwrote the fragment and task with mesh. The surviving mesh binary exactly matches archive key `c493feeca76d7b3d3756193df2766634ca31a2a1a8b8e7b50059349a9be4d790`. Task archive has a unique640-byte candidate `0bf222be05e595d3303e9c923f1954c31eb138283f2cc1a93ac9d54d17acee4c`, not yet proven by byte comparison. Do not assume the zero-ID capture contains all3 stages. Capture was fixed to SHA256(control+code), and MKW_MESH_CAPTURE_DIR saves only the program at the guard. Fix compiled, but the following20second run ended before the guard, so complete recapture still pending. `/private/tmp/mkw-first-mesh-content.log` is that incomplete run.

Next host work: adapt PR983's stage aliases (task6->host3, mesh7->host4) in bindings and program masks; native shader requests need mesh metadata/raw control and full code area (including primitive SPH after instructions), or prepare through Python before the service. NvShader needs mesh-kind/threads/payload/task flags and depth entry selection. Fill driver tables for host3/4 and reflect cp_c0 reads with their proper ranges. Adapt mesh pipeline construction and object/mesh argument tables into our older renderer (upstream nvn_geometry.inc mtk_pso/mtk_encode/mesh_tasks_draw). Avoid copying its whole newer renderer. Pixel pass-through unsigned target variant also belongs to PR983; check it when the first mesh pipeline links. Keep the GPU guard and bounded full-game authorization.


## Latest continuation (2026-11-02 evening): first on-screen picture

The window now shows the game: the world map with the Mario Kart World logo at frame ~200 (window capture via
TOTK_WINDOW_SHOT), and frame 600's 2560x1440 scene target holds a 3D desert course. Textures are badly corrupted
(blocky/dithered patches, magenta streaks), so this is NOT correct or playable rendering. Cache identity is now
`mkw-sm86-v8-mesh-unverified`; the service build in use is `/private/tmp/mkw-ampere-service-mesh` (rebuilt from
tools/ampere_service). Mesh pipelines build and draw (host wiring was already done before this session).

Fixes, in the order they were found (all uncommitted):
- Virtual-pool holes (totk_vpool.c hole_fill): residency wires every page of a zero-copy buffer, including
  untouched zero-fill pages (measured 2 GiB -> +2049 MB wired). Holes are now views of one 64 MB scratch region.
  probe-vpool has a 1 GiB sparse-pool wiring check (44 checks pass).
- Huge sparse textures (nvn_mtl.m tex_new_res): 8192x8192x68 BC6H (5.8 GB) and ASTC (1.45 GB) arrays in virtual
  pools are allocated at the smallest size that keeps their mip count (2048x2048). Metal sparse textures with no
  tiles mapped page-fault when sampled through Metal 4 argument buffers (measured), so TOTK_SPARSE_BIG=2 (sparse)
  is not usable. Contents are not the game's: tile mappings are not followed. Real fix: Metal sparse tiles mapped
  from MapVirtual requests.
- Virtual-pool remaps no longer drop the textures in the pool (mtl_pool_del_keep_textures): every streaming
  MapVirtual rebuilt 341 MB textures and leaked GB.
- Image views (image_fast_rid) are no longer added to the residency set: they were never removed and kept
  every view and parent alive (5041 views in 40 s).
- Pool zero-copy buffers get a 64 KiB tail over the following guest pages when those are RW (MKW_POOL_TAIL=0
  off), against dynamic uniform-block reads past a pool's end. Not proven to be needed.
- Crash handler (runtime/loader.c): the TOTK stale-reference workarounds read GAME_ADDR(0x15ecb0c) =
  0xffffffff on MKW, faulted inside the handler and looped forever, hiding the real fault and stalling the game.
- GPU-executed indexed indirect draws (gpu_indirect_on, MKW_GPU_INDIRECT=0 off): readback stalls were ~90 % of
  the replay thread; frames went from ~290 ms to 12-80 ms. Base instance (c0+0x644) reads 0 for these draws;
  counted multi-draws, 8-bit indices and compute dispatches still read back. No dedicated GPU probe yet.
- Vertex clip z (tools/ampere_service/Service.cs): the game uses NEAR_IS_MINUS_W, the external frontend ignored
  depth_minus_w; the service now writes z=(z+w)/2 at every return of the vertex entry point. This was why the
  window stayed black (swapchain copy at z=-1). Mesh stages already converted (wrapper main0/main0_z).

Still open / observed:
- Intermittent guest SIGBUS (~1 in 10 runs) writing the alias region at main+0x702c40 (region RW at report time);
  not reproduced standalone. Investigate stack_move / alias decommit races.
- Memory: ~9.5 GB wired by the game at the loading screen even with TOTK_MIP_DROP=1 (mirrored textures 4.7 GB,
  pools 3.2 GB). Runs use TOTK_MIP_DROP=1 on this 16 GB Mac. The texture corruption may partly come from mip drop
  and the shrunken sparse textures; compare without them.
- ~50 % of runs still stall early (frame ~169) for reasons not yet identified after the handler fix; retry.
- Progress needs TOTK_ASYNC_SHADERS=1 and a warm cache; cold v8 reaches frame ~300 in 40 s.

Testing: scripts/run_bounded_gpu.py <log> <seconds> [ENV=value ...] (paths in it are this machine's temp layout) samples system wired memory every 0.25 s and kills the process group above
RUN_WIRED_LIMIT_MB (12000 used; earlier kernel panics were at ~15 GB wired), wakes the display with caffeinate
(the game fails "native display link initialization" when the display sleeps), can take `footprint`/`sample`
snapshots, and runs were extended to 40 s on that basis. Replace build/mkw-native with cp + mv (an in-place cp
gets SIGKILLed by code-signing). Diagnostics added (all env-gated): MKW_POOL_LOG (pool/residency/alloc sites),
MKW_FAULT_TRACE (+TOTK_CHUNK_DRAWS=1 TOTK_CHUNK_FIRST=1, DM_RES_CHECK=1) names the work in a faulting command
buffer, MKW_MESH_SKIP / MKW_MESH_DEBUG, MKW_SKIP_CS=<key>, MKW_WIN_TRACE=<frame>. Useful existing ones:
DM_NVN_PASSDUMP=<frame>, DM_NVN_TEXDUMP=<frame>:<path%d>, TOTK_WINDOW_SHOT=<frames>:<path%d>
(DM_NVN_SHOT captures at the game's present call, before async replay: use TOTK_WINDOW_SHOT).
