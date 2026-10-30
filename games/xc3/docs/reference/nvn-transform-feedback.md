# NVN transform feedback: the program layout and its hardware form

Found 2026-10-28 by reading the NVN driver in XC3 1.0.0's own `sdk` module (NVN 12.3.9) and checking it against the game's first TF program. Addresses are in that `sdk` (build ID `E8CDCEB1…`). No driver code is copied: this page describes data layouts and behaviour, and our code will implement them independently.

## Where it lives
NVN has no API call that sets the transform feedback layout. GLSLC puts it in each shader's **control section**, and the driver reads it during `nvnProgramSetShaders`.
- The control-section accessors are one-line getters at `sdk+0x470e7c…0x471148`, called through a vtable whose TF entries are at slots `+0xf8…+0x130`.
- `sdk+0x471678` copies the TF fields into the driver's program object.
- `sdk+0x4902d4…0x4904c4` turns them into Maxwell 3D methods.

## Control-section fields (per shader; 4 buffers, 128 entries each)

| Offset | Type | Meaning |
|---|---|---|
| `+0x0dc + b*0x80 + k` | s8 | output id A of entry k of buffer b (see the table below) |
| `+0x2dc + b*0x80 + k` | u8 | first component B of that output (0 = x) |
| `+0x4dc + b*0x80 + k` | u8 | number of components C |
| `+0x6dc + b*2` | u16 | stride of buffer b in bytes; 0 = buffer unused |
| `+0x6e4 + b` | u8 | stream (the hardware `BufferIndex`) |
| `+0x6e8 + b` | u8 | number of entries in buffer b |
| `+0x6ec`, `+0x6ed` | u8 | 1 and 1 in the TF program seen (meaning not checked yet; probably "TF in use") |

## What the driver programs (Maxwell 3D)
For each buffer b with a non-zero stride:
- **TFB state** (`0x700 + b*0x10`):
  - `BufferIndex` = stream,
  - `VaryingsCount` = stride / 4,
  - `Stride` = stride.
- **Varying locations** (128 bytes per buffer). Walk the entries in order and append one byte per component:
  - for k < count[b], for c = B … B+C−1: `loc = table[A] + c`;
  - an A of −6…−3 is a gap: append 0xff C times;
  - zero-fill to a multiple of 4.
  Components are packed in entry order, so entry k's first component lands right after entry k−1's last.

`table[A]` (a byte per output id, `sdk+0xad221c`, 2-byte stride) gives the hardware attribute word, in the units Ryujinx uses (`location * 4` = attribute byte address):

| A | location | attribute |
|---|---|---|
| 0 | 0x1c | position (0x70) |
| 32 + n (n = 0…31) | 32 + 4n | generic attribute n (0x80 + 16n) |
| 1–28, 30 | 0x18–0xe8 | system and fixed-function outputs (point size, layer, clip distances, colours, texture coordinates…), as in the dump: `28,160,164,168,172,186,27,192,196,200,204,208,212,216,220,224,228,176,177,178,179,180,181,182,183,191,26,25,232,255,24,255` |

## Checked on the game
XC3's first TF program is a single vertex shader with no fragment stage, so rasterization is off.
- Its SPH (type 1) has `OmapGenericVector[0] = 0xf`: it writes generic attribute 0, xyzw, and not the position.
- Its control section has A = 32, B = 0, C = 4, stride 16, stream 0, count 1.
- That gives locations 32, 33, 34, 35: generic 0, x to w, packed in 16 bytes. This matches the shader.

Reading the SPH right matters: in the VTG layout the output map starts at byte 0x32 (`OmapSystemValuesA` 3 bytes, `OmapSystemValuesB`, then `OmapGenericVector` from 0x36).

## How the emulation uses it (done 2026-10-28)
The Ryujinx translator emulates TF on hosts without it (`EmitterContext.PrepareForVertexReturn`): the vertex shader stores the captured components to storage buffers. It asks:
- `QueryTransformFeedbackEnabled()`,
- `QueryTransformFeedbackVaryingLocations(b)`, the 128 bytes above,
- `QueryTransformFeedbackStride(b)`,

and reads `tfe_offset[b]` and `tfe_vertex_count` from its support buffer (MSL bytes 1360 and 1376: `int4 _m6` and `int _m7` after `float4 _m5[73]` at 192, checked in a translated TF shader).
- **Translation.** `nvn_mtl.m rt_answers_for` computes `tfb` (stride and 128 locations per buffer) from a vertex shader's control section. `tools/totk_xlate` answers the queries from it (`Program.cs`) and keys the variant on it (`Service.cs` deps, `totk_shader_rt.m deps_match`).
- **Buffers.** Patch `0008` puts the `tfe_dataN` buffers in descriptor set 0 at binding 1000+N, the set the layer binds. They are reflected as kind `tfe` (`NVN_RB_TFE`).
- **Recording.** Bind, Begin, End, Pause, Resume and Draw become `OP_TF_BIND` / `OP_TF_CTL` records (`hle_nvn.c`).
- **Replay** (`nvn_mtl.m g_tf`):
  - a capturing draw binds the buffers and gets its own support-buffer copy with the offsets and the vertex count; the offsets then advance by vertices × instances × stride;
  - under `SetRasterizerDiscard` it runs with rasterization off (`PSO_NORASTER`) and a vertex function made to return void (`vs_fn_void`);
  - End and Pause keep the bytes per control address; DrawTransformFeedback draws bytes / stride of vertex stream 0.
- **Limits** (logged): capture is exact for point, line and triangle lists drawn without indices; strips and indexed draws are captured per vertex.
