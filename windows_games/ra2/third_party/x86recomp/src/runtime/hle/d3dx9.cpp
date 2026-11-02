// d3dx9_35: the D3DX 9 functions games import, on the d3d9 HLE.
//
// Textures from files in memory: DDS (2D and cube; DXT1-5, the common RGB,
// luminance and alpha layouts, and D3DFORMAT FourCCs). Missing mip levels
// are generated with a box filter for uncompressed formats. Anything
// else (other image formats, resizing, format conversion) is fatal and
// named, to be added when a game needs it.
//
// Shaders: D3DXCompileShader translates HLSL to GLSL (runtime/hlsl) and
// returns it in a shader function Create*Shader recognise
// (d3d9::TranslatedFunction), with an ID3DXConstantTable of the registers
// the translation placed the uniforms in.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/hle/d3d9_impl.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using namespace d3d9;
using namespace guest;

constexpr uint32_t D3DX_DEFAULT = 0xFFFFFFFF;
constexpr uint32_t D3DX_DEFAULT_NONPOW2 = 0xFFFFFFFE;
constexpr uint32_t D3DX_FROM_FILE = 0xFFFFFFFD;
constexpr uint32_t D3DFMT_FROM_FILE = 0xFFFFFFFD;
constexpr uint32_t D3DXERR_INVALIDDATA = 0x88760B59;
constexpr uint32_t D3DXIFF_DDS = 4;

struct D3DXIMAGE_INFO {
  uint32_t Width, Height, Depth, MipLevels, Format, ResourceType, ImageFileFormat;
};
static_assert(sizeof(D3DXIMAGE_INFO) == 28);

// DDS_HEADER after the "DDS " magic.
struct DdsHeader {
  uint32_t size, flags, height, width, pitch_or_linear_size, depth, mip_count;
  uint32_t reserved1[11];
  struct {
    uint32_t size, flags, fourcc, rgb_bits, r_mask, g_mask, b_mask, a_mask;
  } pf;
  uint32_t caps, caps2, caps3, caps4, reserved2;
};
static_assert(sizeof(DdsHeader) == 124);

constexpr uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4,
                   DDPF_RGB = 0x40, DDPF_LUMINANCE = 0x20000;
constexpr uint32_t DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_CUBEMAP_ALLFACES = 0xFC00,
                   DDSCAPS2_VOLUME = 0x200000;

struct Image {
  uint32_t width = 0, height = 0, levels = 0, format = 0;
  bool cube = false;
  const uint8_t* data = nullptr;  // faces, each with its mip chain
  uint32_t size = 0;
};

// The D3DFORMAT of a DDS pixel format, or 0.
uint32_t DdsFormat(const DdsHeader& h) {
  const auto& p = h.pf;
  if (p.flags & DDPF_FOURCC) return p.fourcc;  // DXTn or a D3DFORMAT number
  auto masks = [&](uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return p.rgb_bits == bits && p.r_mask == r && p.g_mask == g && p.b_mask == b &&
           ((p.flags & DDPF_ALPHAPIXELS) ? p.a_mask : 0) == a;
  };
  if (p.flags & DDPF_RGB) {
    if (masks(32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)) return D3DFMT_A8R8G8B8;
    if (masks(32, 0xFF0000, 0xFF00, 0xFF, 0)) return D3DFMT_X8R8G8B8;
    if (masks(24, 0xFF0000, 0xFF00, 0xFF, 0)) return D3DFMT_R8G8B8;
    if (masks(16, 0xF800, 0x7E0, 0x1F, 0)) return D3DFMT_R5G6B5;
    if (masks(16, 0x7C00, 0x3E0, 0x1F, 0x8000)) return D3DFMT_A1R5G5B5;
    if (masks(16, 0x7C00, 0x3E0, 0x1F, 0)) return D3DFMT_X1R5G5B5;
    if (masks(16, 0xF00, 0xF0, 0xF, 0xF000)) return D3DFMT_A4R4G4B4;
    if (masks(16, 0xF00, 0xF0, 0xF, 0)) return D3DFMT_X4R4G4B4;
    return 0;
  }
  if (p.flags & DDPF_LUMINANCE) {
    if (p.rgb_bits == 8 && p.r_mask == 0xFF) return D3DFMT_L8;
    if (p.rgb_bits == 16 && p.r_mask == 0xFF && p.a_mask == 0xFF00) return D3DFMT_A8L8;
    return 0;
  }
  if ((p.flags & DDPF_ALPHA) && p.rgb_bits == 8 && p.a_mask == 0xFF) return D3DFMT_A8;
  return 0;
}

uint32_t LevelSize(gfx::Format f, uint32_t w, uint32_t h) {
  return gfx::RowPitch(f, w) * gfx::RowCount(f, h);
}

// Parses a DDS file; false (with *why) if it is not one we read.
bool ParseDds(const uint8_t* p, uint32_t size, Image* img, std::string* why) {
  if (size < 128 || std::memcmp(p, "DDS ", 4) != 0) {
    *why = "not a DDS file";
    return false;
  }
  DdsHeader h;
  std::memcpy(&h, p + 4, sizeof h);
  if (h.size != 124) {
    *why = "bad DDS header size";
    return false;
  }
  if (h.caps2 & DDSCAPS2_VOLUME) {
    *why = "volume DDS";
    return false;
  }
  img->width = h.width;
  img->height = h.height;
  img->levels = std::max(1u, h.mip_count);
  img->format = DdsFormat(h);
  img->cube = (h.caps2 & DDSCAPS2_CUBEMAP) != 0;
  if (img->cube && (h.caps2 & DDSCAPS2_CUBEMAP_ALLFACES) != DDSCAPS2_CUBEMAP_ALLFACES) {
    *why = "cube DDS without all six faces";
    return false;
  }
  gfx::Format f = FormatFromD3D(img->format);
  if (f == gfx::Format::Unknown || f == gfx::Format::P8) {
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "DDS pixel format flags %X fourcc %08X bits %u masks %X/%X/%X/%X",
                  h.pf.flags, h.pf.fourcc, h.pf.rgb_bits, h.pf.r_mask, h.pf.g_mask,
                  h.pf.b_mask, h.pf.a_mask);
    *why = buf;
    return false;
  }
  uint32_t need = 0;
  for (uint32_t i = 0; i < img->levels; i++)
    need += LevelSize(f, std::max(1u, img->width >> i), std::max(1u, img->height >> i));
  if (img->cube) need *= 6;
  if (size - 128 < need) {
    *why = "truncated DDS";
    return false;
  }
  img->data = p + 128;
  img->size = need;
  return true;
}

// The next mip level of an uncompressed image: each pixel the average of
// the 2x2 (or 2x1 / 1x2 at the edges of odd sizes) block above it, per
// channel, through A8R8G8B8.
std::vector<uint8_t> Downsample(const uint8_t* src, uint32_t w, uint32_t h, gfx::Format f) {
  uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
  uint32_t bpp = gfx::BitsPerPixel(f) / 8, pitch = gfx::RowPitch(f, w);
  std::vector<uint8_t> out(size_t(gfx::RowPitch(f, nw)) * nh);
  for (uint32_t y = 0; y < nh; y++)
    for (uint32_t x = 0; x < nw; x++) {
      uint32_t sum[4] = {}, n = 0;
      for (uint32_t dy = 0; dy < 2; dy++)
        for (uint32_t dx = 0; dx < 2; dx++) {
          uint32_t sx = std::min(w - 1, 2 * x + dx), sy = std::min(h - 1, 2 * y + dy);
          uint32_t argb = gfx::DecodePixel(src + size_t(sy) * pitch + size_t(sx) * bpp, f);
          for (int ch = 0; ch < 4; ch++) sum[ch] += (argb >> (8 * ch)) & 0xFF;
          n++;
        }
      uint32_t argb = 0;
      for (int ch = 0; ch < 4; ch++) argb |= ((sum[ch] + n / 2) / n) << (8 * ch);
      gfx::EncodePixel(out.data() + (size_t(y) * nw + x) * bpp, f, argb);
    }
  return out;
}

// The D3DXCreate(Cube)TextureFromFileInMemory(Ex) core.
uint32_t CreateFromMemory(Ctx& c, bool cube, uint32_t device, uint32_t src, uint32_t size,
                          uint32_t width, uint32_t height, uint32_t levels, uint32_t usage,
                          uint32_t format, uint32_t pool, uint32_t color_key,
                          uint32_t info, uint32_t out) {
  Wr32(out, 0);
  if (!EnsureReadable(c, src, size)) return D3DERR_INVALIDCALL;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(GPtr(src));
  Image img;
  std::string why;
  if (!ParseDds(p, size, &img, &why)) {
    char magic[5] = {};
    std::memcpy(magic, p, std::min<uint32_t>(4, size));
    for (char& ch : magic)
      if (ch && (ch < 32 || ch > 126)) ch = '.';
    FatalF("d3dx9: texture from memory: %s (%u bytes, starts \"%s\"): not supported yet",
           why.c_str(), size, magic);
  }
  if (img.cube != cube)
    FatalF("d3dx9: a %s DDS for a %s texture", img.cube ? "cube" : "2D",
           cube ? "cube" : "2D");
  if (info) {
    *G<D3DXIMAGE_INFO>(info) = {img.width, img.height, 1, img.levels, img.format,
                                cube ? D3DRTYPE_CUBETEXTURE : D3DRTYPE_TEXTURE, D3DXIFF_DDS};
  }
  auto from_file = [](uint32_t v) {
    return v == 0 || v == D3DX_DEFAULT || v == D3DX_DEFAULT_NONPOW2 || v == D3DX_FROM_FILE;
  };
  uint32_t w = from_file(width) ? img.width : width;
  uint32_t h = from_file(height) ? img.height : height;
  if (cube) h = w;
  if (w != img.width || h != img.height)
    FatalF("d3dx9: resizing a %ux%u image to %ux%u not supported yet", img.width,
           img.height, w, h);
  uint32_t fmt = (format == D3DFMT_UNKNOWN || format == D3DX_DEFAULT ||
                  format == D3DFMT_FROM_FILE) ? img.format : format;
  if (fmt != img.format)
    FatalF("d3dx9: converting format %X to %X not supported yet", img.format, fmt);
  gfx::Format f = FormatFromD3D(fmt);
  uint32_t full = 1;
  for (uint32_t s = std::max(w, h); s > 1; s >>= 1) full++;
  uint32_t want = levels == D3DX_FROM_FILE ? img.levels
                  : (levels == 0 || levels == D3DX_DEFAULT) ? full
                                                            : std::min(levels, full);
  if (want > img.levels && gfx::IsCompressed(f)) {
    // Generating them would mean decompressing and compressing again.
    WarnF("d3dx9: %ux%u DXT texture with %u of %u mip levels: using %u\n", w, h,
          img.levels, want, img.levels);
    want = img.levels;
  }
  if (color_key)
    FatalF("d3dx9: colour key %08X on a texture from memory not supported yet", color_key);

  Device* d = DeviceOf(device);
  uint32_t tex;
  uint32_t hr = NewTexture(*d, device, cube, w, h, want, usage, fmt, pool, &tex);
  if (hr != D3D_OK) return hr;
  Texture* t = com::As<Texture>(tex);
  const uint8_t* q = img.data;
  for (uint32_t face = 0; face < t->Faces(); face++) {
    std::vector<uint8_t> generated;
    for (uint32_t i = 0; i < img.levels; i++) {
      uint32_t lw = std::max(1u, w >> i), lh = std::max(1u, h >> i);
      if (i < want) FillLevel(*t, face, i, q, gfx::RowPitch(f, lw));
      if (i + 1 == img.levels && want > img.levels)
        generated.assign(q, q + LevelSize(f, lw, lh));
      q += LevelSize(f, lw, lh);
    }
    for (uint32_t i = img.levels; i < want; i++) {
      uint32_t pw = std::max(1u, w >> (i - 1)), ph = std::max(1u, h >> (i - 1));
      generated = Downsample(generated.data(), pw, ph, f);
      FillLevel(*t, face, i, generated.data(), gfx::RowPitch(f, std::max(1u, w >> i)));
    }
  }
  Wr32(out, tex);
  RECOMP_TRACE("  texture from memory: DDS %ux%u x%u fmt %X%s -> %u levels\n", img.width,
               img.height, img.levels, img.format, cube ? " cube" : "", want);
  return D3D_OK;
}

// (pDevice, pSrcData, SrcDataSize, ppTexture)
HLE("d3dx9_35.dll", D3DXCreateTextureFromFileInMemory, 4) {
  return CreateFromMemory(c, false, Arg(c, 0), Arg(c, 1), Arg(c, 2), D3DX_DEFAULT,
                          D3DX_DEFAULT, D3DX_DEFAULT, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, 0,
                          0, Arg(c, 3));
}

// (pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage, Format,
// Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture)
HLE("d3dx9_35.dll", D3DXCreateTextureFromFileInMemoryEx, 15) {
  return CreateFromMemory(c, false, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4),
                          Arg(c, 5), Arg(c, 6), Arg(c, 7), Arg(c, 8), Arg(c, 11), Arg(c, 12),
                          Arg(c, 14));
}

// (pDevice, pSrcData, SrcDataSize, ppCubeTexture)
HLE("d3dx9_35.dll", D3DXCreateCubeTextureFromFileInMemory, 4) {
  return CreateFromMemory(c, true, Arg(c, 0), Arg(c, 1), Arg(c, 2), D3DX_DEFAULT,
                          D3DX_DEFAULT, D3DX_DEFAULT, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, 0,
                          0, Arg(c, 3));
}

// ---- maths -----------------------------------------------------------------------------
// In single precision, operations in the order D3DX evaluates them (built
// without FMA contraction, see CMakeLists.txt): the game's own maths uses
// the results. D3DXMATRIX: 16 floats, row-major, row vectors.

using Mat = std::array<float, 16>;

Mat ReadMatrix(uint32_t va) {
  Mat m;
  std::memcpy(m.data(), GPtr(va), sizeof m);
  return m;
}

void WriteMatrix(uint32_t va, const Mat& m) { std::memcpy(GPtr(va), m.data(), sizeof m); }

struct Vec3 {
  float x, y, z;
};

Vec3 ReadVec3(uint32_t va) {
  Vec3 v;
  std::memcpy(&v, GPtr(va), sizeof v);
  return v;
}

Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
// D3DXVec3Normalize: a zero vector stays zero.
Vec3 Normalize(Vec3 v) {
  float len = std::sqrt(Dot(v, v));
  if (len == 0.0f) return {0, 0, 0};
  return {v.x / len, v.y / len, v.z / len};
}

// (pOut, pM1, pM2): pOut may be either input.
HLE("d3dx9_35.dll", D3DXMatrixMultiply, 3) {
  Mat a = ReadMatrix(Arg(c, 1)), b = ReadMatrix(Arg(c, 2)), r;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] +
                     a[i * 4 + 3] * b[12 + j];
  WriteMatrix(Arg(c, 0), r);
  return Arg(c, 0);
}

// (pOut, pEye, pAt, pUp)
HLE("d3dx9_35.dll", D3DXMatrixLookAtLH, 4) {
  Vec3 eye = ReadVec3(Arg(c, 1)), at = ReadVec3(Arg(c, 2)), up = ReadVec3(Arg(c, 3));
  Vec3 z = Normalize(Sub(at, eye));
  Vec3 x = Normalize(Cross(up, z));
  Vec3 y = Cross(z, x);
  Mat m = {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0,
           -Dot(x, eye), -Dot(y, eye), -Dot(z, eye), 1};
  WriteMatrix(Arg(c, 0), m);
  return Arg(c, 0);
}

// (pOut, w, h, zn, zf)
HLE("d3dx9_35.dll", D3DXMatrixOrthoLH, 5) {
  auto f = [&](int i) {
    uint32_t v = Arg(c, i);
    float x;
    std::memcpy(&x, &v, 4);
    return x;
  };
  float w = f(1), h = f(2), zn = f(3), zf = f(4);
  Mat m{};
  m[0] = 2.0f / w;
  m[5] = 2.0f / h;
  m[10] = 1.0f / (zf - zn);
  m[14] = zn / (zn - zf);
  m[15] = 1.0f;
  WriteMatrix(Arg(c, 0), m);
  return Arg(c, 0);
}

// ---- ID3DXBuffer ------------------------------------------------------------------------

struct XBuffer : com::Object {
  uint32_t mem = 0, size = 0;
  ~XBuffer() override {
    if (mem) HeapFreeProcess(mem);
  }
};

uint32_t XBufferVtable() {
  return com::Vtable("d3dx9_35.dll", "ID3DXBuffer",
                     {"QueryInterface", "AddRef", "Release", "GetBufferPointer", "GetBufferSize"});
}

// A new ID3DXBuffer of `size` bytes, copied from `data` if given (else zero).
uint32_t NewXBuffer(const void* data, uint32_t size) {
  auto b = std::make_shared<XBuffer>();
  b->size = size;
  b->mem = HeapAllocProcess(std::max(size, 1u), true);
  if (data) std::memcpy(GPtr(b->mem), data, size);
  return com::Publish(b, XBufferVtable());
}

XBuffer* XBuf(Ctx& c) {
  auto* b = com::As<XBuffer>(Arg(c, 0));
  if (!b) FatalF("ID3DXBuffer call on unknown object %08X", uint32_t(Arg(c, 0)));
  return b;
}

HLE_NAMED("d3dx9_35.dll", "ID3DXBuffer::AddRef", D3DXBuffer_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("d3dx9_35.dll", "ID3DXBuffer::Release", D3DXBuffer_Release, 1) { return com::Release(Arg(c, 0)); }
HLE_NAMED("d3dx9_35.dll", "ID3DXBuffer::GetBufferPointer", D3DXBuffer_GetBufferPointer, 1) {
  return XBuf(c)->mem;
}
HLE_NAMED("d3dx9_35.dll", "ID3DXBuffer::GetBufferSize", D3DXBuffer_GetBufferSize, 1) {
  return XBuf(c)->size;
}

// (NumBytes, ppBuffer)
HLE("d3dx9_35.dll", D3DXCreateBuffer, 2) {
  if (!Arg(c, 1)) return D3DERR_INVALIDCALL;
  Wr32(Arg(c, 1), NewXBuffer(nullptr, Arg(c, 0)));
  return D3D_OK;
}

// ---- ID3DXConstantTable -----------------------------------------------------------------
// Top-level constants only (the translator has no struct uniforms). A
// D3DXHANDLE is an address inside `handles`, one byte per constant; like
// D3DX, any other value passed as a handle is read as a constant's name.

struct ConstantTable : com::Object {
  uint32_t version = 0;  // the shader's version token
  std::vector<hlsl::Constant> constants;
  uint32_t handles = 0;
  std::vector<uint32_t> names;  // guest strings
  uint32_t creator = 0;
  ~ConstantTable() override {
    if (handles) HeapFreeProcess(handles);
    for (uint32_t n : names) HeapFreeProcess(n);
    if (creator) HeapFreeProcess(creator);
  }
};

uint32_t ConstantTableVtable() {
  return com::Vtable(
      "d3dx9_35.dll", "ID3DXConstantTable",
      {"QueryInterface", "AddRef", "Release", "GetBufferPointer", "GetBufferSize", "GetDesc",
       "GetConstantDesc", "GetSamplerIndex", "GetConstant", "GetConstantByName",
       "GetConstantElement", "SetDefaults", "SetValue", "SetBool", "SetBoolArray", "SetInt",
       "SetIntArray", "SetFloat", "SetFloatArray", "SetVector", "SetVectorArray", "SetMatrix",
       "SetMatrixArray", "SetMatrixPointerArray", "SetMatrixTranspose",
       "SetMatrixTransposeArray", "SetMatrixTransposePointerArray"});
}

uint32_t GuestString(const std::string& s) {
  uint32_t p = HeapAllocProcess(uint32_t(s.size()) + 1, true);
  std::memcpy(GPtr(p), s.c_str(), s.size() + 1);
  return p;
}

uint32_t NewConstantTable(const hlsl::Program& p, uint32_t version) {
  auto t = std::make_shared<ConstantTable>();
  t->version = version;
  t->constants = p.constants;
  t->handles = HeapAllocProcess(std::max<uint32_t>(uint32_t(p.constants.size()), 1), true);
  for (const auto& k : p.constants) t->names.push_back(GuestString(k.name));
  t->creator = GuestString("x86recomp HLSL translator");
  return com::Publish(t, ConstantTableVtable());
}

ConstantTable* Table(Ctx& c) {
  auto* t = com::As<ConstantTable>(Arg(c, 0));
  if (!t) FatalF("ID3DXConstantTable call on unknown object %08X", uint32_t(Arg(c, 0)));
  return t;
}

// The constant a handle (or name) refers to, -1 if none.
int FindConstant(const ConstantTable& t, uint32_t h) {
  if (!h) return -1;
  if (h >= t.handles && h < t.handles + t.constants.size()) return int(h - t.handles);
  std::string name = GStr(h);
  for (size_t i = 0; i < t.constants.size(); i++)
    if (t.constants[i].name == name) return int(i);
  return -1;
}

HLE_NAMED("d3dx9_35.dll", "ID3DXConstantTable::AddRef", D3DXConstTable_AddRef, 1) {
  return com::AddRef(Arg(c, 0));
}
HLE_NAMED("d3dx9_35.dll", "ID3DXConstantTable::Release", D3DXConstTable_Release, 1) {
  return com::Release(Arg(c, 0));
}

// (pDesc): D3DXCONSTANTTABLE_DESC {Creator, Version, Constants}
HLE_NAMED("d3dx9_35.dll", "ID3DXConstantTable::GetDesc", D3DXConstTable_GetDesc, 2) {
  ConstantTable* t = Table(c);
  uint32_t d = Arg(c, 1);
  if (!d) return D3DERR_INVALIDCALL;
  Wr32(d, t->creator);
  Wr32(d + 4, t->version);
  Wr32(d + 8, uint32_t(t->constants.size()));
  return D3D_OK;
}

// (hConstant, pConstantDesc, pCount): D3DXCONSTANT_DESC, 48 bytes each.
HLE_NAMED("d3dx9_35.dll", "ID3DXConstantTable::GetConstantDesc", D3DXConstTable_GetConstantDesc, 4) {
  ConstantTable* t = Table(c);
  int i = FindConstant(*t, Arg(c, 1));
  uint32_t d = Arg(c, 2), count = Arg(c, 3);
  if (i < 0 || !count) return D3DERR_INVALIDCALL;
  if (d && Rd32(count) >= 1) {
    const hlsl::Constant& k = t->constants[i];
    const uint32_t v[12] = {t->names[i], uint32_t(k.set), k.index, k.count, uint32_t(k.cls),
                            uint32_t(k.type), k.rows, k.columns, k.elements, k.struct_members,
                            k.bytes, 0 /* DefaultValue */};
    for (int w = 0; w < 12; w++) Wr32(d + 4 * w, v[w]);
  }
  Wr32(count, 1);
  return D3D_OK;
}

// (hConstant): the sampler register, or -1.
HLE_NAMED("d3dx9_35.dll", "ID3DXConstantTable::GetSamplerIndex", D3DXConstTable_GetSamplerIndex, 2) {
  ConstantTable* t = Table(c);
  int i = FindConstant(*t, Arg(c, 1));
  if (i < 0 || t->constants[i].set != hlsl::RegisterSet::Sampler) return 0xFFFFFFFF;
  return t->constants[i].index;
}

// (hConstant, Index): the Index-th top-level constant when hConstant is
// null.
HLE_NAMED("d3dx9_35.dll", "ID3DXConstantTable::GetConstant", D3DXConstTable_GetConstant, 3) {
  ConstantTable* t = Table(c);
  if (Arg(c, 1)) FatalF("ID3DXConstantTable::GetConstant of a struct member");
  return Arg(c, 2) < t->constants.size() ? t->handles + Arg(c, 2) : 0;
}

// (hConstant, pName)
HLE_NAMED("d3dx9_35.dll", "ID3DXConstantTable::GetConstantByName", D3DXConstTable_GetConstantByName, 3) {
  ConstantTable* t = Table(c);
  if (Arg(c, 1)) FatalF("ID3DXConstantTable::GetConstantByName of a struct member");
  if (!Arg(c, 2)) return 0;
  std::string name = GStr(Arg(c, 2));
  for (size_t i = 0; i < t->constants.size(); i++)
    if (t->constants[i].name == name) return t->handles + uint32_t(i);
  return 0;
}

// ---- shaders --------------------------------------------------------------------------

// <ID>_SHADER_DUMP=dir: every HLSL source D3DXCompileShader gets is
// written to dir/<FNV-1a of profile, entry and source>.hlsl, with the
// profile, entry point and macros in a comment on top (the format
// recomp_hlslc reads).
void DumpShader(const std::string& src, const std::string& profile, const std::string& entry,
                const std::string& defines) {
  const char* dir = Env("SHADER_DUMP");
  if (!dir || !*dir) return;
  uint64_t h = 1469598103934665603ull;
  for (const std::string* s : {&profile, &entry, &src})
    for (unsigned char ch : *s) h = (h ^ ch) * 1099511628211ull;
  char path[512];
  std::snprintf(path, sizeof path, "%s/%016llx.hlsl", dir, (unsigned long long)h);
  if (FILE* f = std::fopen(path, "wb")) {
    std::fprintf(f, "// profile %s, entry %s%s%s\n", profile.c_str(), entry.c_str(),
                 defines.empty() ? "" : ", defines ", defines.c_str());
    std::fwrite(src.data(), 1, src.size(), f);
    std::fclose(f);
  }
}

// (pSrcData, SrcDataLen, pDefines, pInclude, pFunctionName, pProfile,
// Flags, ppShader, ppErrorMsgs, ppConstantTable)
HLE("d3dx9_35.dll", D3DXCompileShader, 10) {
  uint32_t src = Arg(c, 0), len = Arg(c, 1), defs = Arg(c, 2);
  if (!EnsureReadable(c, src, len)) return D3DERR_INVALIDCALL;
  std::string text(reinterpret_cast<const char*>(GPtr(src)), len);
  std::vector<hlsl::Define> macros;
  std::string defines;
  for (uint32_t d = defs; d && Rd32(d); d += 8) {  // D3DXMACRO {Name, Definition}
    hlsl::Define m{GStr(Rd32(d)), Rd32(d + 4) ? GStr(Rd32(d + 4)) : ""};
    if (!defines.empty()) defines += ' ';
    defines += m.name + (m.value.empty() ? "" : "=" + m.value);
    macros.push_back(std::move(m));
  }
  std::string entry = GStr(Arg(c, 4)), profile = GStr(Arg(c, 5));
  uint32_t flags = Arg(c, 6);
  DumpShader(text, profile, entry, defines);
  if (Arg(c, 3)) FatalF("d3dx9: D3DXCompileShader with an include handler not supported yet");
  hlsl::Program prog;
  std::string errors;
  // A source the original compiler accepts and the translator does not is
  // a translator gap: fatal, with the message (dump the source to see it).
  if (!hlsl::Compile(text, macros, entry, profile, flags, &prog, &errors))
    FatalF("d3dx9: D3DXCompileShader(%s, %s, %u bytes, flags %X) failed: %s", profile.c_str(),
           entry.c_str(), len, flags, errors.c_str());
  std::vector<uint32_t> fn = TranslatedFunction(prog);
  if (Arg(c, 7)) Wr32(Arg(c, 7), NewXBuffer(fn.data(), uint32_t(fn.size() * 4)));
  if (Arg(c, 8)) Wr32(Arg(c, 8), 0);  // no messages
  if (Arg(c, 9)) Wr32(Arg(c, 9), NewConstantTable(prog, fn[0]));
  return D3D_OK;
}

}  // namespace
}  // namespace recomp::hle
