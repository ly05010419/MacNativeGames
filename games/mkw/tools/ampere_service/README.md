# MKW Ampere development service

This .NET 10 service adapts nxnative's batch Ampere frontend to the native runtime's line-oriented shader protocol. Service.cs is the repository's MIT-licensed TOTK service adapted for SM86 requests and constant bank 24. It still needs validation of NVN2 resource layouts beyond the startup captures; it is not a supported playable build.

Use nxnative commit `65501fbbaf8526f7aab0a1edd028135888590f8e` (https://github.com/seedds/nxnative) for the external `dmsh_xlate` frontend and SPIRV-Cross. Build this project with `dotnet build -c Release -o <service-output>`. Set these variables when launching the native runtime:

- `MKW_AMPERE_SERVICE`: absolute path to the built `ampere_service` executable.
- `MKW_DMSH_XLATE`: absolute path to nxnative's `dmsh_xlate` executable.
- `MKW_SPIRV_CROSS`: absolute path to SPIRV-Cross.
- `DOTNET_ROOT`: the .NET installation, if the app hosts cannot find it.

Translation runs external processes per shader in this development version. Cache outputs include game shader code and must stay private. The service produced Metal source and reflection for all 36 captured startup requests. With `TOTK_GPU_MIDCLEAR=1` (the standalone midclear probe passed 9,216 checks), a native launch reached 174 presented frames before stopping on unimplemented `nvnCommandBufferDrawElementsIndirect` and `nvnCommandBufferDispatchComputeIndirect`. Presented frames have not been visually verified. This does not verify gameplay or complete resource binding correctness.

Indirect indexed draws, counted multi-draws and compute dispatches now have a conservative replay path that stages their arguments on the GPU timeline and waits before ordinary replay. `probe-gpu-indirect` checks GPU copy/update visibility across chunk splits (13 values checked). This path stalls per readback and still needs full game validation. The latest game run recorded these calls and then stopped at unimplemented `SetStencilCullCriteria`; `DrawMeshTasks` was also observed reaching an NVN2 stub.

Correction to the frame counts above: these full-game launches did not set `TOTK_GPU_ALLOW_SUBMIT=1`. The inherited containment guard therefore encoded commands without submitting them to the GPU. Their present counts establish CPU/replay progress only, not GPU rendering. Standalone GPU probes explicitly enable submissions and remain valid. The guard documents four prior GPU-driver kernel panics; a full-game submission test must account for that known risk.

MKW SDK disassembly locates stencil-cull criteria at sdk+0x5e9270 (function, reference, mask) and mesh tasks at sdk+0x5e2e80 (two 32-bit arguments). The first observed mesh-task call is (0, 1), so it cannot be discarded on the basis of its first argument being zero. Mesh rendering remains unsupported.

The loose dump's six `Shader/*.bfsha.zs` archives contain 12,788 vertex/pixel stage references (6,394 program variants), deduplicating to 3,257 shader keys. `tools/extract_bnsh.py <shader-directory> <private-output>` extracts these for `prepare_ampere_jobs.py`. The batch frontend emitted SPIR-V for all 3,257, but 1,038 attribute-offset diagnostics reported unsupported offset 0x5C. The runtime service rejects invalid-attribute diagnostics rather than accepting the frontend's substituted values. The meaning and lowering of this attribute still need investigation. No task/mesh stage was identified by the archive stage descriptors in this set.

The v2 chain identifies 0x5C as the per-primitive shading-rate output (Mesa NAK). Since this path uses full-rate shading, the service replaces scalar static vertex AST stores to that output with NOPs, preserving instruction addresses and scheduling fields. Other forms still fail. Sixteen previously rejected shaders passed the service and Metal compiler after this rewrite; only the sixteen targeted stores changed. This is compilation evidence, not visual equivalence or gameplay validation. `tests/check_vprs_service.py <jobs.json> <service> <spirv-cross>` repeats the service and byte-preservation checks against a private archive corpus; set `MKW_DMSH_XLATE` and `DOTNET_ROOT` as for the service.

The extractor also reads BAHS members of `Shader/Drive.Nin_Ounce_NVN2.release.sarc.zs`. With this archive included, the set contains 4,230 unique shaders: 1,107 vertex, 2,626 pixel, 472 compute, and 25 tagged task/mesh (control stage 6 or 7). `prepare_ampere_jobs.py` explicitly rejects the 25 task/mesh shaders and two unclassified graphics shaders; it prepares 4,203 ordinary stage jobs. These counts describe the inspected dump, not complete coverage of the game's shaders. Task/mesh translation and Metal execution remain unimplemented.

The full affected set was subsequently checked: all 1,038 archive shaders that originally reported unsupported attribute 0x5C passed the v2 service, and all 1,038 emitted MSL sources compiled through the Mac's `newLibraryWithSource` path. This still does not validate their rendered pixels, graphics pipeline interfaces, or task/mesh support.

A user-approved 20-second full-game test with `TOTK_GPU_ALLOW_SUBMIT=1` subsequently completed its time limit without a driver crash. GPU timing samples were nonzero (about 0.4–0.5 ms around present notifications 169–178), but the captured native window showed a black game area and the debug overlay. No game imagery or gameplay was verified. This single short run does not establish stability under later GPU load. The test was terminated and its service process group cleaned up.

`DrawMeshTasks` now records its two arguments as an explicit command, and replay reports the missing task/mesh implementation instead of silently dropping it. The command-recording probe includes the observed `(0, 1)` request. Actual task/mesh translation and execution remain pending.

Black-frame investigation found a concrete failed memory path in the native startup: a 250 MiB (`0x0fa00000`) physical pool with flags `0x18c` (physical, compressible, CPU cached, GPU inaccessible) has only 8 KiB alignment and is rejected by the 16 KiB Darwin aliasing requirement. The associated virtual pool (`0x2a1`, also 250 MiB) then maps an invalid physical-pool object. This is a candidate cause of missing image data, not yet a proved explanation for all black rendering. Any fix must preserve CPU/virtual/GPU coherence and must not replace already-published guest GPU addresses. The diagnostic launch kept GPU submission disabled and stopped at the indirect readback requirement.

The unaligned-pool path now supports a complete contiguous mapping to CPU-only physical backing before the virtual pool's address or resources are published. It retains the original CPU storage, never copies it, refuses replacement after publication, and refuses later changes to a borrowed mapping. The vpool probe passed 39 CPU/GPU checks, including GPU writes visible in the original unaligned CPU storage and a published-address rejection. MKW accepted the path; the physical-pool initialization and virtual-map errors disappeared. A bounded GPU run progressed to frame 208 and then exited on unimplemented Ampere compute opcode 0x59F at offset 0xC0. No screenshot was captured before that exit, so resolution of the black frame remains unverified.

The v3 candidate lowers the encountered direct 2D SURED.ADD.U32 form to the frontend's discarded-result image atomic path. The previously failing frame-208 shader now translates and compiles with Metal; other surface dimensions remain unsupported. `tests/check_sured_service.py <private-job.json> <service> <spirv-cross>` checks byte preservation and rejects a deliberately unsupported variant. Image atomic modules use native MSL 3.1 texture atomics, not a raw shadow pointer; writable R32 integer textures request shader-atomic usage. The synthetic `tests/run_atomic_probe.py <service> <spirv-cross>` test passed actual Metal GPU readback: 64 invocations each add one to an R32Uint image pixel, leaving adjacent pixels unchanged. A subsequent bounded native run passed the old shader failure and aborted on a different compute shader reading unsupported driver constants at c0+0x0; gameplay remains unverified. See [HANDOFF.md](../../HANDOFF.md) for current development state and test paths.


The v4 candidate maps only compute c0+4 scalar reads to the replay's dispatch-X count. Its meaning is inferred from the encountered shader's grid normalization; console driver layout remains unverified. Synthetic GPU readback verifies that canonicalization consumes the existing count correctly, while other header scalars remain unsupported. The formerly failing captured shader passes reflection. Keep cache identity v4 separate from v3.


The subsequent v5 candidate extends that scalar mapping to c0+8 as dispatch Y, based on the next kernel's corresponding Y normalization. Both captured failures now pass reflection. The synthetic GPU test checks X and Y against distinct counts and rejects header bytes0/12. Cache v5 is current; this still does not independently prove the console driver layout or gameplay output.


Task/mesh translation now accepts compute jobs from `prepare_ampere_jobs.py` carrying `mesh_layout`, `mesh_stage`, and `shared`. Use the external frontend from nxnative PR983 (`b124b24`) and set `MKW_MESH_STAGE_WRAPPER` to the absolute path of `tools/wrap_mesh_stage.py` (`MKW_PYTHON` optionally overrides python3). Reflections identify ts/ms, object/mesh entry kinds, metadata, and storage tables at c0+0x610/+0x710. The separate service build at `/private/tmp/mkw-ampere-service-mesh` was verified on all25 archive task/mesh jobs. Native requests and host mesh pipelines are not wired yet; the game still stops at the mesh guard. Mesh experiments used an isolated `mkw-sm86-v6-mesh-unverified` output directory, while the current native runtime remains v5.

Synthetic rendering check:
```
python3 games/mkw/tests/make_mesh_probe.py /private/tmp/mkw-mesh-gpu-probe
clang -fobjc-arc -framework Foundation -framework Metal games/mkw/tests/mesh_probe.m -o /private/tmp/mkw-mesh-gpu-probe/check
/private/tmp/mkw-mesh-gpu-probe/check /private/tmp/mkw-mesh-gpu-probe
```
Run from the repository root with Metal access. This renders no game content.
