#pragma once
// Backend-neutral rendering device with Direct3D 7 fixed-function
// semantics. The ddraw/d3d7 and OpenGL HLE talk only to this; backends
// translate:
//   - d3d9 via dxvk-native (gfx/d3d9_device.cpp): Linux bring-up and the
//     reference image;
//   - SDL_GPU with fixed-function uber-shaders (gfx/gpu_device.cpp):
//     Metal on macOS; Vulkan on Linux once its shaders exist.
// Numeric enums (render states, texture stage states, transform types,
// primitive types, FVF codes) are D3D7's; render states D3D7 lacks use
// D3D9's numbers (COLORWRITEENABLE 168, SCISSORTESTENABLE 174,
// SLOPESCALEDEPTHBIAS 175, DEPTHBIAS 195), and FVFs may use D3D9's
// XYZW position (0x4002: homogeneous, untransformed). Plain data structs mirror the
// D3D7 ones (which D3D9 also kept unchanged).
//
// Direct3D 9's programmable pipeline (shaders translated from HLSL,
// runtime/hlsl) is a second way to draw: CreateShader / DrawShader, with
// up to four render targets (SetRenderTargets). Its render states are
// D3D9's (also COLORWRITEENABLE1-3 190-192, BLENDFACTOR 193,
// SEPARATEALPHABLENDENABLE 206 and the alpha blend 207-209). The d3d9
// backend does not have it.

#include <cstdint>
#include <vector>
#include <memory>

#include "runtime/hlsl/hlsl.h"

struct SDL_Window;

namespace recomp::gfx {

enum class Format : uint32_t {
  Unknown, X8R8G8B8, A8R8G8B8, R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4,
  X4R4G4B4, R3G3B2, A8, L8, A8L8, P8,
  D16, D24X8, D24S8, D32, D15S1,
  DXT1, DXT2, DXT3, DXT4, DXT5,
  // Floating point, in D3D9's memory layout (red first): uploaded and
  // stored as is.
  R16F, G16R16F, A16B16G16R16F, R32F, G32R32F, A32B32G32R32F,
};
uint32_t BitsPerPixel(Format f);
bool IsCompressed(Format f);
bool IsFloat(Format f);
// Bytes per row of `width` pixels (per row of 4x4 blocks if compressed).
uint32_t RowPitch(Format f, uint32_t width);
// Rows in the data for `height` (block rows if compressed).
uint32_t RowCount(Format f, uint32_t height);

// Single-pixel conversion between uncompressed colour formats and 32-bit
// A8R8G8B8 (missing alpha reads as 0xFF). For blits and readbacks.
uint32_t DecodePixel(const uint8_t* p, Format f);
void EncodePixel(uint8_t* p, Format f, uint32_t argb);
// Raw pixel value (as stored) for colour keys and fills.
uint32_t RawPixel(const uint8_t* p, Format f);
void PutRawPixel(uint8_t* p, Format f, uint32_t raw);

struct Rect {
  int32_t left, top, right, bottom;
};

// An overlay image (Device::SetOverlayImage id) placed at x, y (w x h) in
// the drawable's pixels, faded by alpha.
struct OverlayItem {
  int image;
  float x, y, w, h;
  float alpha;
};

struct Viewport {
  uint32_t x, y, width, height;
  float min_z, max_z;
};

struct Color {
  float r, g, b, a;
};

struct Material {
  Color diffuse, ambient, specular, emissive;
  float power;
};

struct Light {
  uint32_t type;
  Color diffuse, specular, ambient;
  float position[3], direction[3];
  float range, falloff, attenuation0, attenuation1, attenuation2, theta, phi;
};

using TextureId = uint32_t;  // 0: none
using DepthId = uint32_t;    // 0: none
using ShaderId = uint32_t;   // 0: none

// A colour target: a mip level of a render-target texture, or the back
// buffer (tex 0, level 0). A level is max(1, size >> level) on each side.
struct ColorTarget {
  TextureId tex = 0;
  int level = 0;
};

// ---- the programmable pipeline (Direct3D 9 shaders translated from HLSL)

// Where a vertex shader input (hlsl::Program::inputs slot) comes from: a
// stream, the byte offset in its vertices and the D3DDECLTYPE; type 17
// (D3DDECLTYPE_UNUSED) if the vertex declaration does not supply it.
struct VertexInput {
  uint32_t stream = 0, offset = 0, type = 17;
};

struct VertexStream {
  const uint8_t* data = nullptr;  // vertex 0 (the buffer at its stream offset)
  uint32_t size = 0;              // bytes readable from data
  uint32_t stride = 0;
};

// A Direct3D 9 draw with shaders. Vertex v of a stream is at data +
// v * stride; v is index + base_vertex (indexed) or base_vertex + i, i <
// vertex_count (not indexed). Indexed draws read only vertices
// base_vertex + min_index .. + vertex_count - 1.
struct ShaderDraw {
  ShaderId vs = 0, ps = 0;
  uint32_t prim = 0;        // D3DPRIMITIVETYPE
  uint32_t primitives = 0;  // primitive count
  const VertexInput* inputs = nullptr;  // one per vertex shader input slot
  uint32_t input_count = 0;
  VertexStream streams[16];
  int32_t base_vertex = 0;
  uint32_t min_index = 0, vertex_count = 0;
  const void* indices = nullptr;  // null: not indexed
  uint32_t index_size = 2;        // bytes
  // Constant registers: float c (vs 256, ps 224), int i (16 x 4), bool b
  // (16).
  const float (*vs_float)[4] = nullptr;
  const int32_t (*vs_int)[4] = nullptr;
  const uint32_t* vs_bool = nullptr;
  const float (*ps_float)[4] = nullptr;
  const int32_t (*ps_int)[4] = nullptr;
  const uint32_t* ps_bool = nullptr;
  // Samplers 0-15, then the vertex samplers 0-3: texture (0: none) and
  // D3DSAMPLERSTATETYPE values (index 1-13).
  TextureId textures[20] = {};
  const uint32_t (*sampler_states)[14] = nullptr;
};

class Device {
 public:
  virtual ~Device() = default;

  // (Re)creates the swap chain: a w x h back buffer presented to the
  // window. Called on SetDisplayMode / device creation.
  virtual void ResetSwapChain(int w, int h, bool vsync) = 0;

  // ---- resources
  virtual TextureId CreateTexture(int w, int h, int levels, Format fmt,
                                  bool render_target) = 0;
  virtual void DestroyTexture(TextureId t) = 0;
  // Fills `r` of a render-target texture's top level with an A8R8G8B8
  // colour (DirectDraw colour-fill Blt; its memory is GPU-only).
  virtual void FillTexture(TextureId t, const Rect& r, uint32_t argb) = 0;
  // Uploads `r` of mip `level` from linear memory (pitch in bytes).
  virtual void UpdateTexture(TextureId t, int level, const Rect& r,
                             const void* src, uint32_t pitch) = 0;
  // A cube texture: six square faces in D3D's order (+X, -X, +Y, -Y, +Z,
  // -Z), filled with UpdateCubeTexture and freed with DestroyTexture.
  virtual TextureId CreateCubeTexture(int size, int levels, Format fmt) = 0;
  virtual void UpdateCubeTexture(TextureId t, int face, int level, const Rect& r,
                                 const void* src, uint32_t pitch) = 0;
  virtual DepthId CreateDepth(int w, int h, Format fmt) = 0;
  virtual void DestroyDepth(DepthId d) = 0;

  // ---- render target (color 0 = back buffer; Present copies it to the
  // front buffer and shows that)
  virtual void SetRenderTarget(TextureId color, DepthId depth) = 0;
  // Up to four colour targets drawn at once (shaders' COLOR0-3): color[0]
  // as SetRenderTarget's (or a level of it), the others texture levels or
  // tex 0 (none, and none after the first one).
  virtual void SetRenderTargets(const ColorTarget color[4], DepthId depth) = 0;
  // Copies `sr` of a colour target to `dr` of another, scaled with linear
  // or nearest filtering (Direct3D 9's StretchRect).
  virtual void StretchRect(ColorTarget src, const Rect& sr, ColorTarget dst, const Rect& dr,
                           bool linear) = 0;

  // ---- 2D (DirectDraw Blt onto the back buffer / render target)
  // Copies `src_rect` of a linear image into `dst_rect` of the current
  // render target, stretching if the sizes differ. If `colorkey`, source
  // pixels equal to `key` (in the source format) are skipped.
  virtual void Blit(const void* src, uint32_t pitch, Format fmt,
                    const Rect& src_rect, const Rect& dst_rect, bool colorkey,
                    uint32_t key) = 0;
  // Draws `src_rect` of an A8R8G8B8 image over `dst_rect` of the current
  // render target, blended by the image's alpha (source over, straight
  // alpha). For what a host hook draws in place of a game's own CPU
  // blending onto read-back pixels (a software cursor), which would make
  // the CPU wait for the frame's GPU work.
  virtual void BlitBlend(const void* src, uint32_t pitch,
                         const Rect& src_rect, const Rect& dst_rect) = 0;
  // Reads `r` of the current render target into linear memory.
  virtual void ReadBack(const Rect& r, void* dst, uint32_t pitch,
                        Format fmt) = 0;
  // ReadBack in two steps, so the CPU need not wait for the GPU:
  // StartReadBack takes `r` of the current render target as the draws so
  // far leave it and returns a ticket (never 0); FinishReadBack waits for
  // that copy and converts it as ReadBack does, or only drops it if `dst`
  // is null. Every ticket is finished exactly once.
  virtual uint32_t StartReadBack(const Rect& r) = 0;
  virtual void FinishReadBack(uint32_t ticket, void* dst, uint32_t pitch,
                              Format fmt) = 0;

  // ---- front buffer (the DirectDraw primary): the last presented frame,
  // which the game may also read and draw on between frames (its cursor).
  virtual void BlitFront(const void* src, uint32_t pitch, Format fmt,
                         const Rect& src_rect, const Rect& dst_rect) = 0;
  virtual void ReadBackFront(const Rect& r, void* dst, uint32_t pitch,
                             Format fmt) = 0;
  // Shows the front buffer as it is now.
  virtual void PresentFront() = 0;

  // ---- overlay (gfx/overlay.h): images drawn over every presentation,
  // in the drawable's pixels. Backends without one ignore it.
  virtual void SetOverlayImage(int id, const uint32_t* rgba, int w, int h) {
    (void)id, (void)rgba, (void)w, (void)h;
  }
  virtual void SetOverlayItems(std::vector<OverlayItem> items) { (void)items; }
  // Frames shown on the window so far (for a frame-rate display).
  virtual uint64_t FramesShown() const { return 0; }

  // ---- fixed-function state (D3D7 enums)
  virtual void SetRenderState(uint32_t state, uint32_t value) = 0;
  virtual void SetTextureStageState(uint32_t stage, uint32_t type,
                                    uint32_t value) = 0;
  virtual void SetTexture(uint32_t stage, TextureId t) = 0;
  virtual void SetTransform(uint32_t type, const float m[16]) = 0;
  virtual void SetViewport(const Viewport& v) = 0;
  // In render-target pixels; used while SCISSORTESTENABLE (174) is set.
  virtual void SetScissor(const Rect& r) = 0;
  virtual void SetMaterial(const Material& m) = 0;
  virtual void SetLight(uint32_t index, const Light& l) = 0;
  virtual void LightEnable(uint32_t index, bool enable) = 0;
  virtual void SetClipPlane(uint32_t index, const float plane[4]) = 0;

  // ---- drawing
  virtual void Clear(uint32_t count, const Rect* rects, uint32_t flags,
                     uint32_t color, float z, uint32_t stencil) = 0;
  virtual void BeginScene() = 0;
  virtual void EndScene() = 0;
  // Non-indexed when `indices` is null. `stride` from the FVF.
  virtual void Draw(uint32_t prim, uint32_t fvf, const void* vertices,
                    uint32_t vertex_count, uint32_t stride,
                    const uint16_t* indices, uint32_t index_count) = 0;
  // Indexed draw built in place: MapDraw returns room for `vertex_count`
  // vertices (`stride` bytes each) and `index_count` indices in memory the
  // device reads them from, for the caller to fill and then draw with
  // DrawMapped before the next MapDraw. Saves Draw's copy for vertices
  // made per draw.
  struct Mapped {
    uint8_t* vertices;
    uint16_t* indices;
  };
  virtual Mapped MapDraw(uint32_t vertex_count, uint32_t stride,
                         uint32_t index_count) = 0;
  virtual void DrawMapped(uint32_t prim, uint32_t fvf) = 0;
  virtual void Present() = 0;

  // ---- programmable pipeline. A shader from the HLSL translation (null
  // on failure); render states (blend, depth, stencil, cull, colour
  // write, alpha test, scissor), the viewport and the targets as set
  // above, the rest per draw.
  virtual ShaderId CreateShader(const hlsl::Program& p) = 0;
  virtual void DestroyShader(ShaderId s) = 0;
  virtual void DrawShader(const ShaderDraw& d) = 0;
};

// Where a src_w x src_h frame is shown in a dst_w x dst_h output: scaled
// to fit with its aspect kept and centred, black around it (letterbox or
// pillarbox). Backends present the front buffer through it; the window
// system maps the pointer back through it.
Rect FitRect(int src_w, int src_h, int dst_w, int dst_h);

// The device presenting to the window (the last one created), or null:
// for on-screen controls drawn by the window system's side.
Device* PresentingDevice();
void SetPresentingDevice(Device* d);

// The colour of the bars around a fitted frame (default black; a dev
// switch can make them stand out). Components 0..1.
void SetBarColor(float r, float g, float b);
void BarColor(float* r, float* g, float* b);

// A strip at the frame's right edge (a fixed-width side panel, e.g. a
// sidebar of buttons) shown `scale` times as large as the rest of the
// frame (GameConfig::side_panel): its top src_h / scale rows fill the
// picture's height beside the rest, which is shown as it would be in a
// frame width * (scale - 1) wider. While `active` is null or returns false
// the frame is shown whole (FitRect). Width 0 or scale <= 1: no panel.
// With it, a bar along the frame's bottom left of the panel (e.g. a row of
// buttons) can be shown bar_scale times as large over the main part's
// bottom left corner: its bottom bar_height rows, the left bar_length of
// the main part's width. bar_height 0 or bar_scale <= 1: no bar.
struct SidePanel {
  int width = 0;
  float scale = 1;
  bool (*active)() = nullptr;
  int bar_height = 0;
  float bar_length = 1;
  float bar_scale = 1;
};
void SetSidePanel(const SidePanel& p);
// Rows at the frame's bottom not shown (GameConfig::crop_bottom): while
// `active` is null or returns true, and no side panel is shown, the frame's
// top src_h - rows fill the picture (FitRect); input maps to those rows.
void SetCropBottom(int rows, bool (*active)());
// The rows cropped now (asks `active`).
int CropBottomShown();
// Whether the next frame is shown with the panel (asks `active`): once per
// presented frame, by the backend.
bool SidePanelShown();
// The width a src_w-wide frame takes when shown with the panel (src_w
// without one): what a host window sized for the frame should be.
int ShownWidth(int src_w);

// Where a src_w x src_h frame is shown in a dst_w x dst_h output: `fit`
// the picture (FitRect's result, of the wider frame with the panel); the
// frame's main part (all of it without the panel) from main_src to
// main_dst; with the panel, panel_src (its top rows) to panel_dst, right of
// main_dst; with the bar, bar_src to bar_dst, drawn over main_dst's bottom
// left corner.
struct FrameLayout {
  Rect fit, main_src, main_dst;
  bool panel = false;
  Rect panel_src, panel_dst;
  bool bar = false;
  Rect bar_src, bar_dst;
};
FrameLayout LayoutFrame(int src_w, int src_h, int dst_w, int dst_h, bool panel);

// The last frame any device showed, in pixels: the frame's size, the
// output's (swap chain), the picture's rectangle and whether the side
// panel was shown. Zeros before the first.
struct Presentation {
  int src_w, src_h, out_w, out_h;
  Rect rect;
  bool panel = false;
};
// Backends call this when they show the front buffer.
void NotePresentation(const Presentation& p);
Presentation LastPresentation();

// Vertex size in bytes for a D3D7 FVF code.
uint32_t FvfStride(uint32_t fvf);
uint32_t PrimitiveCountFromVertices(uint32_t prim_type, uint32_t verts);

enum class Backend {
  D3D9,  // dxvk-native; only in builds with RECOMP_HAVE_D3D9
  Gpu,   // SDL_GPU
};
// Null if `b` is not in this build or cannot start on this host.
std::unique_ptr<Device> CreateDevice(Backend b, SDL_Window* window);

std::unique_ptr<Device> CreateD3D9Device(SDL_Window* window);
std::unique_ptr<Device> CreateGpuDevice(SDL_Window* window);

}  // namespace recomp::gfx
