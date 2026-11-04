# Native MKW reference notes

PokeDinner-a-Eglantine-Fork was inspected at `393a851` on branch master:
https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork/commits/branch/master

- `b41c41d32e1a209573b6e8f8df4627af4020240d`: MoltenVK shader output typing must handle unused render targets. The native port's `rt_answers` already skips null `R.rt.color` entries, so this patch is not directly applicable.
- `2d14cee8f1efa770c9befb6ba2823a4c43cc3a8c`: Switch 2 built-in font types use the SharedFontType numbering (0 through 5), passed through by the SDK. The new font service returns decoded TrueType/OpenType data, rather than the BFTTF wrapper expected by the older shared-font path. The native port's shared_font.c still implements the older path; data ownership and SDK interfaces need checking before adding the Switch 2 path.
- `1dd40d8`: merges an Apple silicon hypervisor execution backend. This is a separate execution architecture from the native port's rewritten game code; its existence does not establish Metal shader compatibility.

The fork's GPL graphics implementation is a reference for behavior, not vendored into the native MIT runtime. Neither of the two focused changes above resolves the unsupported attribute 0x5C diagnostics found in the native Ampere frontend.

Vivsi1/Eden-NS2-Android was inspected at `1278c6a7c1d8f0e8af98586c2acb63895ca2379d`:
https://github.com/Vivsi1/Eden-NS2-Android
Its README reports MKW 1.8.0 playable with slow loading and missing task/mesh geometry. It uses the Eden Vulkan path; this is not evidence of complete task/mesh support for Metal.

The attribute 0x5C diagnosis comes from Mesa NAK, commit `b5b73e5964bee5d9cdc2b7e813e791d948094e75`: `nak_private.h` names it VPRS_TABLE_INDEX, and `nak_nir.c` maps PRIMITIVE_SHADING_RATE to it on SM86. The native full-rate shading path removes only scalar static AST stores to that output, leaving other attributes and instruction addresses unchanged. Other unsupported attribute accesses still fail.


### User-supplied nxnative PRs (checked 2026-11-02)

- Main HEAD remains `65501fbbaf8526f7aab0a1edd028135888590f8e`.
- [PR #983](https://github.com/seedds/nxnative/pull/983), pinned HEAD `b124b24` (mesh implementation `5ee98b6`, prerequisite Ampere changes `7390446`). MIT. `tools/dmsh_meshstage.py` is vendored from that revision with its copyright/license retained. Its compute stage-memory layout and Metal object/mesh wrapper are used by prepare_ampere_jobs.py and wrap_mesh_stage.py. No GPL frontend was imported.
- [PR #981](https://github.com/seedds/nxnative/pull/981) supplies Ampere fixes; #983 includes its rebased prerequisite. Keep the older external frontend intact for comparison, and use the separate PR checkout for mesh jobs.
- [PR #1003](https://github.com/seedds/nxnative/pull/1003) changes only Bananza's GPU timestamp profile and docs. Relevant performance context, not a mesh implementation. Do not blindly apply a different game's profile.

Local evidence: all25 private task/mesh shaders translate with the PR frontend, all25 wrapped Metal libraries compile, and our service produces all25 with no unfillable driver reads/unknown storage registers. A synthetic GPU test (tests/make_mesh_probe.py + mesh_probe.m) checks the task number -> payload -> mesh output -> raster chain and passes256 pixels. This is not native game integration: the runtime's mesh replay guard remains in place.

### kartnative rendering integration (2026-11-02)

Inspected https://github.com/pahorema/kartnative at
`47f2d1d5c8298a562190dd0493da8a4eb18e4626` (MIT, copyright 2026 seedds;
notice retained in `third_party/d2r_gfx/KARTNATIVE_LICENSE`). Imported its
`nvn_tex.c` sparse storage geometry (without the unrelated texture-buffer helper),
`nvn_sparse_tex.h/.m`, and adapted `tests/sparse_tex.m`. The host adapter uses
this port's per-64-KiB physical-page table and upload batch. CPU and GPU GOB
addressing now exchange Switch 2 offset bits 5/6; GPU region conversion also
uses the padded sparse row width. Shared physical tiles, per-layer mip tails,
and GPU-written sparse targets are implemented by the imported component.

Standalone actual GPU checks: sparse module 36/36, including mapped/unmapped
tiles, rewritten pages, tails, cross-texture physical sharing, GPU-written
R8 contents and lost-page diagnostics. The independent fixed GOB fixture checks
2,560 bytes across CPU read/write, GPU upload and GPU region read/write.
These checks do not establish correct full-game rendering.
