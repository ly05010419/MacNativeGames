# NVN coverage

How many NVN entry points the game loads through `nvnBootstrapLoader`, compared with how many `third_party/d2r_gfx/gfx/nvn/` implements. This feeds the README's "What the game uses" section, in the same form as `games/by3/README.md`.

- NVN API version reported by the game:
- Names loaded:
- Implemented:
- Called at least once before the title screen:

## Missing entry points the game calls

| Name | First called at | Workaround / status |
|---|---|---|
| `nvnTextureBuilderSetDepthStencilMode(STENCIL)` | loading, boot 4 | done: stencil-plane view (`X32_Stencil8`) |
| `nvnCommandBufferClearColori` / `ClearColorui` | after the first button press, boot 14 | done: the ClearColor record, values exact up to 2^24 |
| `nvnCommandBuffer…TransformFeedback…` (7 entry points) | after the first button press, boot 15 | done (boot 26): emulated, see `nvn-transform-feedback.md`; the game's capture writes real data |
| Geometry shaders (NVN stage 2) | boot 16 | emulated for point input (geometry as compute, `nvn-geometry-shaders.md`); other input topologies are skipped (logged) |
| Indexed quads | boot 16 | done (boot 21): each quad a,b,c,d becomes a,b,c a,c,d, expanded on the CPU into 32-bit indices in the submit's arena; quads holding the restart index are dropped (logged), GPU-written index buffers stop |

## Formats and features in use

Texture formats, render-target formats, compute passes, queries, sparse textures, and similar features. Add them as they are seen.

| Feature | Where | Status |
|---|---|---|
| Image of a block-compressed texture (0x4e BC7 sRGB, texture 752, 16×64) | boot 16 | done (boot 31): a **fragment** shader encodes BC7. The image is bound to a kept RGBA32Uint (RG32Uint for 64-bit blocks) shadow of blocks (`bc_shadow`), and after the draw or dispatch the blocks are copied into the BC texture through a buffer (`bc_flush`). The texture is marked GPU-owned (no re-upload from guest memory). Its content is not inspected yet. |
| Texture ids above the last texture pool's size | boot 44 | done (boot 46): XC3 initialises a 2048-entry texture pool after its main one; ids are valid up to the largest pool |
| Several texture / sampler pools, switched per command buffer | boot 57 | done (boot 58): each pool has its own id range in the registration tables; binds are translated by the pool set last |
| Vertex shaders storing to memory under rasterizer discard (GPU skinning) | battle, boot 59 | done (boot 65): drawn with rasterization off, with an intra-pass barrier after each such draw |
