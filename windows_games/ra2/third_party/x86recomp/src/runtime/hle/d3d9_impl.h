#pragma once
// Host objects behind the Direct3D 9 interfaces, shared by d3d9.cpp
// (IDirect3D9, the device's core) and d3d9_resource.cpp (surfaces,
// textures, buffers, vertex declarations, shaders, queries).

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/hlsl/hlsl.h"
#include "runtime/hle/com.h"
#include "runtime/hle/d3d9_types.h"
#include "runtime/ctx.h"
#include "runtime/runtime.h"

namespace recomp::d3d9 {

constexpr uint32_t D3D_OK = 0;
constexpr uint32_t D3DERR_NOTFOUND = 0x88760866;
constexpr uint32_t D3DERR_NOTAVAILABLE = 0x8876086A;
constexpr uint32_t D3DERR_INVALIDCALL = 0x8876086C;
constexpr uint32_t S_FALSE = 1;

// D3DFORMAT values
enum : uint32_t {
  D3DFMT_UNKNOWN = 0, D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21,
  D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23, D3DFMT_X1R5G5B5 = 24,
  D3DFMT_A1R5G5B5 = 25, D3DFMT_A4R4G4B4 = 26, D3DFMT_R3G3B2 = 27,
  D3DFMT_A8 = 28, D3DFMT_X4R4G4B4 = 30, D3DFMT_P8 = 41, D3DFMT_L8 = 50,
  D3DFMT_A8L8 = 51, D3DFMT_D16_LOCKABLE = 70, D3DFMT_D32 = 71,
  D3DFMT_D15S1 = 73, D3DFMT_D24S8 = 75, D3DFMT_D24X8 = 77, D3DFMT_D16 = 80,
  D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102, D3DFMT_R16F = 111,
  D3DFMT_G16R16F = 112, D3DFMT_A16B16G16R16F = 113, D3DFMT_R32F = 114,
  D3DFMT_G32R32F = 115, D3DFMT_A32B32G32R32F = 116,
  D3DFMT_DXT1 = 0x31545844, D3DFMT_DXT2 = 0x32545844,
  D3DFMT_DXT3 = 0x33545844, D3DFMT_DXT4 = 0x34545844,
  D3DFMT_DXT5 = 0x35545844,
};
// D3DRESOURCETYPE
enum : uint32_t {
  D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME = 2, D3DRTYPE_TEXTURE = 3,
  D3DRTYPE_VOLUMETEXTURE = 4, D3DRTYPE_CUBETEXTURE = 5,
  D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7,
};
// D3DUSAGE
enum : uint32_t {
  D3DUSAGE_RENDERTARGET = 0x1, D3DUSAGE_DEPTHSTENCIL = 0x2,
  D3DUSAGE_WRITEONLY = 0x8, D3DUSAGE_DYNAMIC = 0x200,
  D3DUSAGE_AUTOGENMIPMAP = 0x400,
  D3DUSAGE_QUERY_SRGBREAD = 0x10000, D3DUSAGE_QUERY_FILTER = 0x20000,
  D3DUSAGE_QUERY_SRGBWRITE = 0x40000,
  D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING = 0x80000,
  D3DUSAGE_QUERY_VERTEXTEXTURE = 0x100000,
  D3DUSAGE_QUERY_WRAPANDMIP = 0x200000,
};
// D3DPOOL
enum : uint32_t {
  D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2,
  D3DPOOL_SCRATCH = 3,
};
// D3DLOCK
enum : uint32_t {
  D3DLOCK_READONLY = 0x10, D3DLOCK_NOOVERWRITE = 0x1000,
  D3DLOCK_DISCARD = 0x2000, D3DLOCK_NO_DIRTY_UPDATE = 0x8000,
};

gfx::Format FormatFromD3D(uint32_t f);
bool IsDepthFormat(gfx::Format f);
// Formats a render target can have (a swap chain: IsBackBufferFormat).
bool IsRenderTargetFormat(uint32_t f);
bool IsBackBufferFormat(uint32_t f);

template <typename T>
T* G(uint32_t va) {
  return reinterpret_cast<T*>(GPtr(va));
}

struct Texture;

struct Surface : com::Object {
  enum class Kind {
    BackBuffer,    // the swap chain's: gfx render target 0
    DepthStencil,  // gfx depth buffer
    TextureLevel,  // a level of `texture` (its refcount is the texture's)
    RenderTarget,  // CreateRenderTarget: a gfx render-target texture
    Offscreen,     // CreateOffscreenPlainSurface: memory only
  };
  Kind kind = Kind::BackBuffer;
  uint32_t device = 0;  // IDirect3DDevice9 guest pointer
  guest::D3DSURFACE_DESC desc{};
  gfx::DepthId depth = 0;     // DepthStencil
  gfx::TextureId tex = 0;     // RenderTarget
  Texture* texture = nullptr;  // TextureLevel
  uint32_t container = 0;      // TextureLevel: the texture's guest pointer
  int face = 0, level = 0;
  uint32_t mem = 0;  // Offscreen: its pixels
  uint32_t pitch = 0;
  ~Surface() override;
};

struct Texture : com::Object {
  uint32_t device = 0;
  bool cube = false;  // IDirect3DCubeTexture9: six faces of levels each
  uint32_t width = 0, height = 0, levels = 0, usage = 0, format = 0, pool = 0;
  gfx::Format fmt = gfx::Format::Unknown;
  gfx::TextureId tex = 0;  // none for SYSTEMMEM/SCRATCH
  struct Level {
    uint32_t w = 0, h = 0, pitch = 0, size = 0;
    uint32_t mem = 0;  // lock shadow, allocated on first lock
    bool locked = false;
    gfx::Rect locked_rect{};
    bool lock_readonly = false;
    std::shared_ptr<Surface> surface;  // GetSurfaceLevel, created lazily
  };
  std::vector<Level> lv;  // face * levels + level
  Level& At(uint32_t face, uint32_t level) { return lv[face * levels + level]; }
  uint32_t Faces() const { return cube ? 6 : 1; }
  uint32_t lod = 0, priority = 0, autogen_filter = 2;  // D3DTEXF_LINEAR
  ~Texture() override;
};

struct Buffer : com::Object {
  uint32_t device = 0;
  bool index = false;
  uint32_t length = 0, usage = 0, pool = 0;
  uint32_t fvf_or_format = 0;  // FVF (vertex) or D3DFORMAT (index)
  uint32_t mem = 0;            // the data, in guest memory
  uint32_t priority = 0;
  ~Buffer() override;
};

struct VertexDecl : com::Object {
  uint32_t device = 0;
  std::vector<guest::D3DVERTEXELEMENT9> elements;  // with the END marker
};

struct Shader : com::Object {
  uint32_t device = 0;
  bool pixel = false;
  std::vector<uint32_t> tokens;  // the function, END token included
  // A function D3DXCompileShader translated (TranslatedFunction), and
  // the gfx shader made from it.
  std::shared_ptr<const hlsl::Program> program;
  gfx::ShaderId id = 0;
  ~Shader() override;
};

struct Query : com::Object {
  uint32_t device = 0;
  uint32_t type = 0;
};

struct Device : com::Object {
  uint32_t d3d = 0;  // IDirect3D9 guest pointer
  guest::D3DDEVICE_CREATION_PARAMETERS creation{};
  guest::D3DPRESENT_PARAMETERS pp{};
  uint32_t hwnd = 0;
  std::unique_ptr<gfx::Device> gfx;
  std::shared_ptr<Surface> backbuffer, auto_depth;
  // Bound objects: guest pointers the device holds a reference to.
  uint32_t render_targets[4] = {};
  uint32_t depth_stencil = 0;
  uint32_t textures[20] = {};  // samplers 0-15, then vertex samplers 0-3
  struct Stream {
    uint32_t buffer = 0, offset = 0, stride = 0, freq = 1;
  };
  Stream streams[16];
  uint32_t indices = 0;
  uint32_t decl = 0, vs = 0, ps = 0;
  uint32_t rs[256] = {};
  uint32_t ss[20][14] = {};  // samplers 0-15 and the 4 vertex samplers
  uint32_t tss[8][33] = {};
  uint32_t fvf = 0;
  guest::BOOL software_vp = 0;
  std::map<uint32_t, std::array<float, 16>> transforms;
  guest::D3DVIEWPORT9 viewport{};
  guest::D3DRECT scissor{};
  float vs_f[256][4] = {}, ps_f[224][4] = {};
  int32_t vs_i[16][4] = {}, ps_i[16][4] = {};
  guest::BOOL vs_b[16] = {}, ps_b[16] = {};
  int scene_depth = 0;
  // Render states as gfx has them (SyncState); all sent on the first sync.
  uint32_t sent_rs[256] = {};
  bool synced = false;
  // The D3D cursor (SetCursorProperties / ShowCursor).
  bool cursor_shown = false, cursor_set = false;
};

// IUnknown::QueryInterface for an object with one interface `iid` (and
// IUnknown): (this, riid, ppvObject).
uint32_t QueryInterfaceFor(Ctx& c, const guest::GUID& iid);

// The device a method's object belongs to (fatal if `self` is not one).
Device* DeviceOf(uint32_t self);

// Sampler index: 0-15, or D3DVERTEXTEXTURESAMPLER0..3 (256..259) -> 16..19;
// -1 if neither.
int SamplerIndex(uint32_t s);

// Replaces a bound object: references the new one, releases the old.
void Bind(uint32_t* slot, uint32_t obj);

// A standalone surface (back buffer, depth stencil, render target,
// offscreen plain): published with refcount 1, kept in *keep if given.
uint32_t NewSurface(Device& d, uint32_t device, Surface::Kind kind, uint32_t w,
                    uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool,
                    std::shared_ptr<Surface>* keep);

// The gfx render targets and depth buffer for the current bindings.
void BindTargets(Device& d);

// Sends gfx the render states that changed, the viewport and the scissor
// rectangle (before draws and clears; d3d9_draw.cpp).
void SyncState(Device& d);

// CreateTexture / CreateCubeTexture (cube: w = h = edge length): the
// HRESULT; the new texture's guest pointer in *out (0 on failure).
uint32_t NewTexture(Device& d, uint32_t device, bool cube, uint32_t w, uint32_t h,
                    uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool,
                    uint32_t* out);

// D3DXCompileShader's output: a shader function holding the HLSL
// translation instead of shader model bytecode. Version token, one comment
// with kTranslatedTag and the serialised hlsl::Program, END. Create*Shader
// recognise it.
constexpr uint32_t kTranslatedTag = 0x504D4352;  // 'RCMP'
std::vector<uint32_t> TranslatedFunction(const hlsl::Program& p);
// The Program in such a function; null for any other function.
std::shared_ptr<const hlsl::Program> ReadTranslated(const std::vector<uint32_t>& tokens);

// Replaces a whole level's pixels (rows of `pitch` bytes, in the
// texture's format) and uploads them: D3DX loaders.
void FillLevel(Texture& t, uint32_t face, uint32_t level, const uint8_t* src,
               uint32_t pitch);

}  // namespace recomp::d3d9
