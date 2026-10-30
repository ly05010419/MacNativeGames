# Vertex storage writes with rasterizer discard

## Symptom and cause

Jamboree 2.3.0 character meshes could stretch into very large triangles. The producer is a Bezel morph/deformation pass: vertex shaders write positions into storage buffers; a later render pass reads those buffers as vertex stream 12. NVN rasterizer discard is enabled for the producer, with transform feedback **disabled**.

The previous Metal path returned early from `tf_draw` when `S.tf_on` was false. It skipped the producer entirely. The later draw therefore consumed uninitialized position data. Rejecting extreme vertices would conceal the failed producer and remove geometry; it does not repair the pass.

Observed writer keys, included as identifiers only: `031c6c0e475e171c`, `6be6273b0719b37c`, `48fba5b566be09f1`. No original shader code, SPIR-V, MSL or compiled game shader libraries are included in this repository.

## Implementation

- `runtime/gfx/tools/dmsh_build.py`: produce `entry_discard` for vertex shaders with writable storage or image-store side effects. The entry returns `void`, as Metal requires for a vertex function with rasterization disabled. All storage writes and early-return behavior survive. The ordinary graphics entry and transform-feedback entry remain available.
- Storage reflection tests bit **0** for `BufferUsageFlags.Write`; bit 1 alone means read, not write. Image stores use their existing flag.
- `runtime/gfx/nvn/nvn_mtl.m`: load the reflected discard entry, use a distinct discard pipeline key, and encode the vertex work without color/depth attachments. Preserve vertex count, first vertex, instance count and base instance. No transform-feedback output buffer is required for this path.
- Track pending GPU storage writes and vertex-buffer epochs so later vertex reads and CPU-visible staging do not reuse stale contents.
- `runtime/gfx/nvn/hle_nvn.c`: invalidate the appropriate bindings when the buffer is rebound after the producer.

The translation chain is `e6e2d522054e`. Its header is generated from the existing chain sources and lock. Game-specific shader-input digest: `37da065929be`. Packages built with a different chain/profile stamp are rejected by packaging and by the runtime's existing checks.

## Regression coverage

`runtime/tools/tests/test_shader_discard.py` translates synthetic MSL wrappers and checks that the void entry preserves stores on both early and final returns. It also tests all read/write flag combinations.

`runtime/gfx/tests/t_geometry.m` exercises synthetic Metal discard vertex writes with feedback off, reads the written data back, checks instancing and bounds, then exercises the existing transform-feedback and geometry paths. `t_vbuf.m` covers vertex buffer bindings and invalidation. Run both through `scripts/test_gpu.py` with Metal validation enabled. These tests contain no game shader code.

A separate private check of one translated morph shader matched 24/24 output values and bounds. It is not included because its shader source derives from the game. The portable regressions and reported scene observations are the shareable evidence.

## Other bring-up requirements

These are kept separate from the generic discard fix:

- Jamboree's build-gated StandardAllocator reservation: assert the original instruction, reduce the virtual reservation from 56 GB to 4 GB. The profile provides a 16 GB alias region; this changes address reservation, not geometry or player state.
- SDK 17.5.9 address-manager reservation cut in `sdk_patches.py`, gated by SDK build ID and original instruction.
- Raise the native NRO module capacity from 128 to 160 for the 139 registered game NROs.
- Read the newer `.bea` ASST layout when splitting shaders.
- Release completed movie frame slots. The end-of-stream continuation bridge is restricted to this game's main build ID; its purpose is to leave the opening movie through the original continuation.
- Persist Mac controller layout choices and handle absent environment variables safely.

No vertex guard, bad-position triangle filter, forced character state, skipped mesh or capture-only instrumentation is enabled or required.
