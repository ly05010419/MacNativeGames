# Geometry shaders on Metal: what XC3 uses and how to emulate it

Metal has no geometry stage. This page records what the game needs (measured 2026-10-28) and the design for emulating it with the vendored Ryujinx translator's "vertex/geometry as compute" path.

## What the game uses (first scene)

Two programs have a geometry stage. `DM_NVN_GS_DUMP=<folder>` saved them in `XC3_build/analysis/gs/`.

| Program (GS key) | VS | GS code | GS output | Max vertices |
|---|---|---|---|---|
| `a841a5d9…` | 128 bytes, reads attribute 0 (xyzw) | 1152 bytes | triangle strip, writes generic outputs 0–2 (`OmapGenericVector 0x33 0x03`) | 4 |
| `95bfab61…` | 128 bytes, reads attribute 0 (xyzw) | 640 bytes | triangle strip | 4 |

Both expand each input primitive (probably a point) into one quad: sprite or particle style. There are 2 draws per frame with them at the first scene, and `DM_NVN_GS_SKIP=1` skips both.

## Translator side (exists in `vendor/ryujinx`)
- **VS as compute:** `TranslatorContext.Translate(asCompute: true)` on the vertex context. It:
  - reads the vertex index from an index-buffer texture (`GetIndexBufferTextureSetAndBinding`) and each attribute from a buffer texture (`GetVertexBufferTextureBinding(location)`);
  - takes counts from `vb_info` (`VertexInfoBuffer`: `vertex_counts` = {vertex count, instance count, first vertex, first instance}, `geometry_counts` = {…, index-buffer base offset}, `vertex_strides[32]`, `vertex_offsets[32]`);
  - writes every output to the vertex-output storage buffer, `OutputSizePerInvocation` words per vertex (`ResourceReservations.Offsets`: position, point size, clip distances, layer, user attributes).
- **GS as compute:** the geometry context gets `SetVertexOutputMapForGeometryAsCompute(vertexContext)`, then `Translate(asCompute: true)`. It reads the VS outputs and writes:
  - emitted vertices to `GeometryVertexOutputStorageBufferBinding`,
  - indices to `GeometryIndexOutputStorageBufferBinding`: strips become lists, with restart.
- **Final draw:** `GenerateVertexPassthroughForCompute()` gives a vertex shader that reads the geometry output by vertex index. The fragment shader stays the game's, translated as usual; its inputs must match the passthrough outputs.
- All of these resources are reserved bindings (`ResourceReservations`, `vertexAsCompute: true`). Like the TF buffers (patch `0008`), they may need moving to descriptor set 0 at fixed high bindings.

## What is missing: the backend (Ryujinx keeps it in `Ryujinx.Graphics.Gpu`, which is not vendored)
It has to be written in the NVN layer. Work items, in order:
1. **Service. Done 2026-10-28.** New request modes for `tools/totk_xlate`:
   - `vsc` (VS as compute, with attribute formats),
   - `gsc` (GS as compute, given the VS code for the output map),
   - `vpt` (passthrough VS for the program).
   Reflection gets new kinds: `vb_info`, vertex-buffer textures per attribute, the index-buffer texture, the topology-remap texture, the vertex-output buffer, and the geometry vertex/index output buffers.
2. **Layer, program setup.** A program with a geometry stage translates three pieces (`vsc`, `gsc`, `vpt`) plus its fragment shader. Draws use them instead of stopping (`prog_get`).
3. **Layer, per draw:**
   - end the render pass, then encode a compute pass:
     - buffer-texture views of the guest vertex buffers per attribute, with element offsets for alignment through `vertex_offsets`,
     - the index buffer, or a sequential one for non-indexed draws,
     - a topology remap for strips and fans,
     - `vb_info`,
     - dispatch VS-as-compute over vertices × instances and GS-as-compute over primitives × instances,
     - output buffers sized from `OutputSizePerInvocation` and the GS maximum vertex count, taken from the submit's arena;
   - reopen the pass and draw indexed with the generated index buffer, the `vpt` vertex function and the fragment shader.
4. **Interaction with the replay's pass optimisations** (merging, deferral, cube groups): a geometry draw must count as a pass boundary in every lookahead.
5. **Transform feedback after a GS** (if the game captures GS output): the TF emulation stores from the vertex stage only (`PrepareForVertexReturn`), so a GS program with TF needs the passthrough VS to capture.

## Step 1, as built
- **Request:** `{"variant", "stage": "vsc"|"gsc"|"vpt", "code", "c1", "topology": "points"|"lines"|"lines_adj"|"triangles"|"triangles_adj", "vs_code" and "vs_c1" (gsc and vpt), tex/img/attr/fout}`. The code is the stage's code after its 0x30-byte header, up to `control+0x704`, as for the other stages.
- **Translation:** `Program.cs TranslateAsCompute`.
  - `vsc` = the VS context's `Translate(asCompute: true)`.
  - `gsc` = the GS context with `SetVertexOutputMapForGeometryAsCompute(vs)` then `Translate(asCompute: true)`.
  - `vpt` = the GS context's `GenerateVertexPassthroughForCompute()`.
  - The kernels are 32×32×1 (`threadgroup_size` in the reflection).
- **Patch `0009`** puts the path's reserved resources in set 0 (`vb_info` 1100, outputs 1200–1202, buffer textures 1300+). SPIRV-Cross numbers argument-buffer ids in order anyway, so the reflection goes by name.
- **Reflection kinds:**
  - `vb_info`,
  - `vtx_out` (`vertex_output` / `vb_input`),
  - `gs_vtx_out`, `gs_idx_out`,
  - `ib_tex`, `trb_tex`,
  - `vb_tex` (reg = attribute).
  The GS part's own resources keep the usual kinds (`cbuf`, `tex`, `samp_tex`, prefix `gp`).
- **Checked offline** on both programs (`XC3_build/analysis/gs/xlate`, requests built from the `DM_NVN_GS_DUMP` files with topology `points`): all six translate, reflect fully, and compile with Metal (`newLibraryWithSource`, MSL 3.0).
- **Open questions for step 3:**
  - `ib_data` and `vb_dataN` are declared `texture_buffer<float>`, so their formats and the index format must match what the kernel reads; check the MSL's reads.
  - The real input topology comes from the draw.

## Steps 2–3, as built (2026-10-28, points only)
- **`prog_get`** keeps a geometry stage's control section and code (`key[2]` set, `sh[2]` nil) instead of stopping. `DM_NVN_GS_SKIP=1` still skips such draws.
- **`draw`** sends a draw of such a program to `vtg_draw` (`nvn_mtl.m`):
  - translates the three parts on first use (`totk_shader_rt.m totk_rt_vtg`, synchronous, cached by variant name);
  - builds the inputs in the submit's arena:
    - u32 vertex indices (identity, or the draw's own) and the identity topology remap, both as R32Float buffer textures (the kernels read the bits);
    - one buffer texture per vertex attribute in its component format (`vtg_attr_format`);
    - `vb_info` = {count, instances, first vertex, first instance}, {primitives, -, -, index base}, strides {elements, has y, has z, has w}, offsets {element offset, per instance};
  - sizes `vertex_output` / `geometry_vb_output` / `geometry_ib_output` from the reflection's `out_words` and the GS SPH (max output vertices `w4 & 0xfff`, invocations `w2 >> 24`);
  - dispatches the two kernels in their own compute encoders (32×32 threads; the geometry kernel binds the geometry stage's state through `g_vtg_compute`);
  - draws an indexed triangle strip (restart 0xffffffff) through `draw_inner` with a program made of the passthrough vertex shader and the fragment shader (`g_vtg_prog`, `g_vtg_ib_gpu`).
- **Checked:** both geometry programs of the first scene run every frame (2304 points each), with no crash and no pipeline or validation error, for 120 s. Whether the quads land in the right place is not checked yet: the screenshots of the menu look the same with and without them.
- **Fixed 2026-10-28 (boots 47–48):** the vertex part gets its own c1 (`vs_c1`; a vertex shader with BRX jump tables failed to decode with the geometry shader's), `vsc` reflects storage buffers against the vertex c0 table (0x110), and the vertex kernel binds vertex-stage state (it used compute-stage state).
- **Not done:**
  - input topologies other than points;
  - transform feedback after a geometry stage;
  - reading back the outputs for a check.

## Estimate
Several days. The steps can be checked one by one:
- each compute pass's output buffers can be dumped and compared with the GS's expected quads (4 vertices per input primitive);
- the final draw shows as sprites in the window screenshots.
