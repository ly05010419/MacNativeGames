# dm/gfx/metal — minimal Metal offscreen example

A standalone small module: it wraps Metal in a pure C interface, renders the picture to an offscreen texture and reads it back to the CPU, without putting anything on screen. It is not compiled into the host (`dm/host/Makefile` does not include this directory); the Metal replay layer the game actually uses is in `../nvn/nvn_mtl.m`. This is kept as a reference that compiles on its own and can be understood in a few dozen lines, and as a small probe for verifying Metal behavior.

## Interface (`mtl_backend.h`)

| Interface | Effect |
|---|---|
| `mtlb_init()` / `mtlb_shutdown(ctx)` | Create / release the context: device, command queue, the current frame's command buffer, pipeline cache |
| `mtlb_target(ctx, w, h, fmt)` / `mtlb_target_free(rt)` | Create / release an offscreen render target, with format `MTLB_FMT_BGRA8` or `MTLB_FMT_RGBA8` |
| `mtlb_clear(ctx, rt, r, g, b, a)` | Records a render pass into the current frame that clears the target to the given color |
| `mtlb_triangle(ctx, rt)` | Records another render pass into the current frame (keeping existing contents) that draws a three-color triangle |
| `mtlb_readback(ctx, rt, buf, size)` | Submits the current frame, waits for the GPU to complete, and reads the pixels back into `buf` tightly packed by row (row 0 is the top of the image) |

Functions return 0 for success and -1 for failure, and the failure reason is printed to standard error. The implementation is in `mtl_backend.m` (Objective-C, ARC).

Design points:

- **One command buffer per frame**: it is created only when a pass is first recorded; subsequent clears and triangles are appended to the same command buffer, and it is submitted only at readback.
- **Render targets are not global variables**: the context and each target are opaque pointers.
- **Storage mode is Shared**: on unified-memory devices a Shared texture can be used directly as a render attachment, and after the GPU completes the CPU reads it directly, with no intermediate buffer needed. On non-unified-memory devices `mtlb_init` reports an error outright.
- The triangle's shader is inline MSL, compiled at run time.

## Building and running

```sh
make -C dm/gfx/metal        # produces test_offscreen
make -C dm/gfx/metal run    # runs the self-test; exit code 0 means all passed
make -C dm/gfx/metal clean
```

Only Xcode's command-line tools are needed; no game files are needed. The build products are already listed in this directory's `.gitignore`.

## Self-test (`test_offscreen.c`)

All expected values are computed independently in the test, without reading the backend's internal data:

- the triangle vertices are copied separately in the test;
- normalized coordinates are converted to pixels: `px = (x + 1) / 2 · W`, `py = (1 − y) / 2 · H`;
- the color at a pixel center is the linear interpolation of the three vertex colors by barycentric coordinates.

The two tests are each run once for both formats, BGRA8 and RGBA8:

1. Clear to a solid color and compare pixel by pixel.
2. Clear the screen and then draw the triangle: pixels inside the triangle (more than 1 pixel from an edge) are compared with the interpolated value, with the tolerance given by `TOL` in the source; pixels outside are compared with the clear color; the 1-pixel band right along the edges is skipped, since rasterization edge rules are outside the scope of this test.

Before readback the buffer is first filled with 0x5a, to prevent "passing even though the backend wrote nothing".

## Probe (`probe/p_nocopy.m`)

A single-file program with no build target; compile it manually with clang when needed. It verifies one thing that the replay layer's memory pool approach depends on: on a piece of memory that is not page-aligned, after extending its start and end outward to page boundaries, a zero-copy `MTLBuffer` can be created; two such buffers are allowed to overlap; and what the GPU writes can be read by the CPU through the original pointer.

## Limitations

- The triangle vertices are hard-coded in `mtl_backend.m`; there is no configurable state such as vertex buffers, shaders or viewports.
- `mtlb_readback` waits synchronously for the whole frame to complete and blocks the calling thread.
- Only a single color attachment and two 8-bit formats are supported; there is no depth / stencil, multisampling or multiple attachments.
- Not thread-safe; only unified-memory devices are supported.
