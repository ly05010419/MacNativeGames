// OpenGL 1.x on gfx::Device (see context.h).
//
// Conventions that differ between GL and the device, handled here:
// - Window coordinates: GL's origin is bottom-left, the device's top-left
//   (viewport, scissor, glReadPixels rows).
// - Clip space: GL's z runs -w..w, the device's 0..w; the projection is
//   rescaled. D3D9 also samples pixel centres half a pixel off from GL,
//   which the projection compensates for, so 2D drawn texel-exactly in GL
//   stays texel-exact.
// - Matrices: GL's column-major storage of M is the device's row-major
//   storage of M^T, which is what its row-vector convention needs, so
//   modelview and projection are passed as stored. The modelview is the
//   device's WORLD and VIEW is identity: the device's world space is GL's
//   eye space, where GL keeps light positions and clip planes and computes
//   fog. Texture matrices act on (s,t,0,1) in GL and (u,v,1,0) on the
//   device; their rows are rearranged.
// - Texture environments map to texture stage states. Textures are stored
//   as A8R8G8B8 (alpha 1 for formats without alpha) or as DXT blocks; GL's
//   per-base-format environment rules are applied when building stages.
// - Winding: both APIs judge it as the triangle appears on screen (GL in
//   its y-up window space, the device in its y-down screen space). GL's
//   front faces are counter-clockwise; the device's are clockwise, so a
//   GL front face is a device back face.

#include "runtime/gl/context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <numeric>
#include <type_traits>

#include "runtime/log.h"

namespace recomp::gl {

namespace {

// Device (D3D) numbering used below.
enum : uint32_t {
  RS_ZENABLE = 7, RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14,
  RS_ALPHATESTENABLE = 15, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22,
  RS_ZFUNC = 23, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_ALPHABLENDENABLE = 27,
  RS_FOGENABLE = 28, RS_SPECULARENABLE = 29, RS_FOGCOLOR = 34,
  RS_FOGTABLEMODE = 35, RS_FOGSTART = 36, RS_FOGEND = 37, RS_FOGDENSITY = 38,
  RS_STENCILENABLE = 52, RS_STENCILFAIL = 53, RS_STENCILZFAIL = 54,
  RS_STENCILPASS = 55, RS_STENCILFUNC = 56, RS_STENCILREF = 57,
  RS_STENCILMASK = 58, RS_STENCILWRITEMASK = 59, RS_TEXTUREFACTOR = 60,
  RS_LIGHTING = 137, RS_AMBIENT = 139, RS_FOGVERTEXMODE = 140,
  RS_COLORVERTEX = 141, RS_LOCALVIEWER = 142, RS_NORMALIZENORMALS = 143,
  RS_DIFFUSEMATERIALSOURCE = 145, RS_SPECULARMATERIALSOURCE = 146,
  RS_AMBIENTMATERIALSOURCE = 147, RS_EMISSIVEMATERIALSOURCE = 148,
  RS_CLIPPLANEENABLE = 152,
  RS_COLORWRITEENABLE = 168, RS_SCISSORTESTENABLE = 174,
  RS_SLOPESCALEDEPTHBIAS = 175, RS_DEPTHBIAS = 195,
};
enum : uint32_t {
  TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4,
  TSS_ALPHAARG1 = 5, TSS_ALPHAARG2 = 6, TSS_TEXCOORDINDEX = 11,
  TSS_ADDRESSU = 13, TSS_ADDRESSV = 14, TSS_MAGFILTER = 16, TSS_MINFILTER = 17,
  TSS_MIPFILTER = 18, TSS_MAXANISOTROPY = 21, TSS_TEXTURETRANSFORMFLAGS = 24,
  TSS_COLORARG0 = 26, TSS_ALPHAARG0 = 27,
};
enum : uint32_t {
  TOP_DISABLE = 1, TOP_SELECTARG1 = 2, TOP_MODULATE = 4, TOP_MODULATE2X = 5,
  TOP_MODULATE4X = 6, TOP_ADD = 7, TOP_ADDSIGNED = 8, TOP_ADDSIGNED2X = 9,
  TOP_SUBTRACT = 10, TOP_BLENDTEXTUREALPHA = 13, TOP_DOTPRODUCT3 = 24,
  TOP_LERP = 26,
};
enum : uint32_t {
  TA_DIFFUSE = 0, TA_CURRENT = 1, TA_TEXTURE = 2, TA_TFACTOR = 3,
  TA_COMPLEMENT = 0x10, TA_ALPHAREPLICATE = 0x20,
};
enum : uint32_t {
  FVF_XYZ = 0x002, FVF_XYZW = 0x4002, FVF_NORMAL = 0x010, FVF_DIFFUSE = 0x040,
  PT_POINTLIST = 1, PT_LINELIST = 2, PT_LINESTRIP = 3, PT_TRIANGLELIST = 4,
  PT_TRIANGLESTRIP = 5, PT_TRIANGLEFAN = 6,
  TS_VIEW = 2, TS_PROJECTION = 3, TS_TEXTURE0 = 16, TS_WORLD = 1,
  CLEAR_TARGET = 1, CLEAR_ZBUFFER = 2, CLEAR_STENCIL = 4,
};

constexpr uint32_t kUnset = 0xDEADBEEF;

uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

uint8_t ToByte(float f) {
  return uint8_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f));
}

uint32_t Argb(const float c[4]) {
  return uint32_t(ToByte(c[3])) << 24 | uint32_t(ToByte(c[0])) << 16 |
         uint32_t(ToByte(c[1])) << 8 | ToByte(c[2]);
}

std::array<float, 16> Identity() {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

// a * b in GL's column-major storage (the product applied as a(b(v))).
std::array<float, 16> Mul(const float* a, const float* b) {
  std::array<float, 16> r;
  for (int c = 0; c < 4; c++)
    for (int row = 0; row < 4; row++) {
      double s = 0;
      for (int k = 0; k < 4; k++) s += double(a[k * 4 + row]) * b[c * 4 + k];
      r[c * 4 + row] = float(s);
    }
  return r;
}

bool IsIdentity(const std::array<float, 16>& m) { return m == Identity(); }

uint32_t CmpFunc(Enum f) { return f >= NEVER && f <= ALWAYS ? f - NEVER + 1 : 8; }

uint32_t BlendFactor(Enum f) {
  switch (f) {
    case ZERO: return 1;
    case ONE: return 2;
    case SRC_COLOR: return 3;
    case ONE_MINUS_SRC_COLOR: return 4;
    case SRC_ALPHA: return 5;
    case ONE_MINUS_SRC_ALPHA: return 6;
    case DST_ALPHA: return 7;
    case ONE_MINUS_DST_ALPHA: return 8;
    case DST_COLOR: return 9;
    case ONE_MINUS_DST_COLOR: return 10;
    case SRC_ALPHA_SATURATE: return 11;
    default: return 2;
  }
}

uint32_t StencilOpOf(Enum op) {
  switch (op) {
    case KEEP: return 1;
    case ZERO: return 2;
    case REPLACE: return 3;
    case INCR: return 4;  // GL's INCR/DECR saturate
    case DECR: return 5;
    case INVERT: return 6;
    default: return 1;
  }
}

int TypeSize(Enum t) {
  switch (t) {
    case BYTE: case UNSIGNED_BYTE: return 1;
    case SHORT: case UNSIGNED_SHORT: return 2;
    case INT: case UNSIGNED_INT: case FLOAT: return 4;
    case DOUBLE: return 8;
    default: return 0;
  }
}

// One array component as GL reads it (normalize: colours and normals).
struct NoType {};

template <class T>
float Load(const uint8_t* p, bool normalize) {
  if constexpr (std::is_same_v<T, NoType>) {
    return 0;
  } else {
    T v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::is_same_v<T, uint8_t>) return normalize ? v / 255.0f : float(v);
    if constexpr (std::is_same_v<T, int8_t>) return normalize ? (2 * v + 1) / 255.0f : float(v);
    if constexpr (std::is_same_v<T, int16_t>) return normalize ? (2 * v + 1) / 65535.0f : float(v);
    if constexpr (std::is_same_v<T, uint16_t>) return normalize ? v / 65535.0f : float(v);
    return float(v);
  }
}

// Calls f(T()) with T the C type of array type t (NoType: other values,
// whose components read as 0).
template <class F>
void ForType(Enum t, F&& f) {
  switch (t) {
    case BYTE: return f(int8_t());
    case UNSIGNED_BYTE: return f(uint8_t());
    case SHORT: return f(int16_t());
    case UNSIGNED_SHORT: return f(uint16_t());
    case INT: return f(int32_t());
    case UNSIGNED_INT: return f(uint32_t());
    case FLOAT: return f(float());
    case DOUBLE: return f(double());
    default: return f(NoType());
  }
}

float ReadComponent(const uint8_t* p, Enum t, bool normalize) {
  float r = 0;
  ForType(t, [&](auto x) { r = Load<decltype(x)>(p, normalize); });
  return r;
}

// Writes one attribute of each device vertex (`dstride` apart from `dst`)
// from source element vs[k] of an array: fn(element, out).
template <class Fn>
void Gather(const uint8_t* base, size_t stride, const std::vector<uint32_t>& vs,
            uint8_t* dst, uint32_t dstride, Fn fn) {
  for (uint32_t v : vs) {
    fn(base + size_t(v) * stride, dst);
    dst += dstride;
  }
}

void Fill(const void* value, uint32_t bytes, size_t n, uint8_t* dst, uint32_t dstride) {
  for (size_t k = 0; k < n; k++, dst += dstride) std::memcpy(dst, value, bytes);
}

// Device primitive for a GL one (0: not supported) and the number of
// device indices for n GL vertices.
uint32_t DevicePrim(Enum mode) {
  switch (mode) {
    case POINTS: return PT_POINTLIST;
    case LINES: return PT_LINELIST;
    case LINE_STRIP: case LINE_LOOP: return PT_LINESTRIP;
    case TRIANGLES: case QUADS: return PT_TRIANGLELIST;
    case TRIANGLE_STRIP: case QUAD_STRIP: return PT_TRIANGLESTRIP;
    case TRIANGLE_FAN: case POLYGON: return PT_TRIANGLEFAN;
    default: return 0;
  }
}

size_t DeviceIndexCount(Enum mode, size_t n) {
  if (mode == QUADS) return n / 4 * 6;
  if (mode == LINE_LOOP && n) return n + 1;
  return n;
}

void ExpandIndices(Enum mode, const std::vector<uint16_t>& seq, uint16_t* out) {
  if (mode == QUADS) {
    for (size_t q = 0; q + 3 < seq.size(); q += 4)
      for (size_t k : {0, 1, 2, 0, 2, 3}) *out++ = seq[q + k];
    return;
  }
  std::copy(seq.begin(), seq.end(), out);
  if (mode == LINE_LOOP && !seq.empty()) out[seq.size()] = seq[0];
}

// The elements glDrawElements' indices refer to, each once in first-use
// order (verts), and each index as a position in that list (seq): only
// the vertices a draw uses are converted, however far apart they lie.
// remap holds epoch << 16 | position per element - lo. false: more than
// 65536 vertices.
template <class T>
bool Compact(const uint8_t* indices, size_t count, std::vector<uint32_t>& remap,
             uint32_t& epoch, std::vector<uint32_t>& verts, std::vector<uint16_t>& seq) {
  auto at = [indices](size_t i) {
    T v;
    std::memcpy(&v, indices + i * sizeof(T), sizeof v);
    return uint32_t(v);
  };
  uint32_t lo = UINT32_MAX, hi = 0;
  for (size_t i = 0; i < count; i++) {
    lo = std::min(lo, at(i));
    hi = std::max(hi, at(i));
  }
  size_t range = size_t(hi - lo) + 1;
  if (range > (size_t(1) << 24)) return false;
  if (range > remap.size()) remap.resize(range);
  if (++epoch == 0x10000) {
    std::fill(remap.begin(), remap.end(), 0);
    epoch = 1;
  }
  // Locals, so the loop does not reload what its stores might alias.
  uint32_t* rm = remap.data();
  verts.resize(std::min(count, size_t(65536)));
  uint32_t* out = verts.data();
  seq.resize(count);
  uint16_t* sq = seq.data();
  uint32_t e = epoch << 16, n = 0;
  for (size_t i = 0; i < count; i++) {
    uint32_t v = at(i);
    uint32_t& r = rm[v - lo];
    if ((r & 0xFFFF0000u) != e) {
      if (n == 65536) return false;
      r = e | n;
      out[n++] = v;
    }
    sq[i] = uint16_t(r);
  }
  verts.resize(n);
  return true;
}

int MaxLevels(int w, int h) {
  int n = 1;
  while ((w >> n) > 0 || (h >> n) > 0) n++;
  return n;
}

bool UsesMipmaps(Enum min_filter) {
  return min_filter != NEAREST && min_filter != LINEAR;
}

}  // namespace

// ---- texture objects -------------------------------------------------------

struct Context::Texture {
  gfx::TextureId id = 0;
  int w = 0, h = 0, levels = 0;
  bool compressed = false;
  gfx::Format fmt = gfx::Format::Unknown;
  // GL base-format facts the environment rules depend on (storage always
  // has four channels).
  bool has_alpha = true, has_color = true;
  std::vector<bool> defined;
  Enum min_filter = NEAREST_MIPMAP_LINEAR, mag_filter = LINEAR;
  Enum wrap_s = REPEAT, wrap_t = REPEAT;
  float aniso = 1;
  bool Complete() const {
    if (!id || defined.empty() || !defined[0]) return false;
    if (!UsesMipmaps(min_filter)) return true;
    return std::all_of(defined.begin(), defined.end(), [](bool b) { return b; });
  }
};

struct Context::Unit {
  uint32_t bound = 0;
  bool tex2d = false;
  Enum env_mode = MODULATE;
  float env_color[4] = {0, 0, 0, 0};
  Enum combine_rgb = MODULATE, combine_alpha = MODULATE;
  Enum src_rgb[3] = {TEXTURE, PREVIOUS, CONSTANT};
  Enum op_rgb[3] = {SRC_COLOR, SRC_COLOR, SRC_ALPHA};
  Enum src_alpha[3] = {TEXTURE, PREVIOUS, CONSTANT};
  Enum op_alpha[3] = {SRC_ALPHA, SRC_ALPHA, SRC_ALPHA};
  float rgb_scale = 1, alpha_scale = 1;
};

Context::Context(gfx::Device* dev, int fb_w, int fb_h)
    : dev_(dev), fb_w_(fb_w), fb_h_(fb_h), units_(new Unit[kUnits]) {
  for (auto& s : stacks_) s.assign(1, Identity());
  std::fill(std::begin(rs_cache_), std::end(rs_cache_), kUnset);
  for (auto& s : tss_cache_) std::fill(std::begin(s), std::end(s), kUnset);
  textures_[0] = new Texture;  // the default texture object
  vp_[2] = scissor_[2] = fb_w;
  vp_[3] = scissor_[3] = fb_h;
  current_.color = 0xFFFFFFFF;
  current_.pos[3] = 1;
  current_.normal[2] = 1;
  depth_ = dev_->CreateDepth(fb_w, fb_h, gfx::Format::D24S8);
  dev_->SetRenderTarget(0, depth_);
  dev_->SetTransform(TS_VIEW, Identity().data());
  for (float* c : {lights_[0].diffuse, lights_[0].specular}) std::fill(c, c + 4, 1.0f);
}

Context::~Context() {
  for (auto& [n, t] : textures_) {
    if (t->id) dev_->DestroyTexture(t->id);
    delete t;
  }
  if (depth_) dev_->DestroyDepth(depth_);
  delete[] units_;
}

void Context::Resize(int fb_w, int fb_h) {
  if (fb_w == fb_w_ && fb_h == fb_h_) return;
  fb_w_ = fb_w;
  fb_h_ = fb_h;
  dev_->ResetSwapChain(fb_w, fb_h, vsync_);
  if (depth_) dev_->DestroyDepth(depth_);
  depth_ = dev_->CreateDepth(fb_w, fb_h, gfx::Format::D24S8);
  dev_->SetRenderTarget(0, depth_);
  matrices_dirty_ = true;
}

void Context::SetVsync(bool on) {
  if (on == vsync_) return;
  vsync_ = on;
  dev_->ResetSwapChain(fb_w_, fb_h_, vsync_);
}

void Context::EndFrame() { scene_ = false; }

// ---- errors ------------------------------------------------------------------

void Context::Error(Enum e, const char* what) {
  if (error_ == NO_ERROR) error_ = e;
  if (warned_.insert({what, e}).second) WarnF("gl: %s: error %04X\n", what, e);
}

void Context::Unsupported(const char* what, Enum v) {
  if (error_ == NO_ERROR) error_ = INVALID_ENUM;
  if (warned_.insert({what, v}).second)
    WarnF("gl: %s %04X not supported\n", what, v);
}

Enum Context::GetError() {
  Enum e = error_;
  error_ = NO_ERROR;
  return e;
}

const char* Context::GetString(Enum name) const {
  switch (name) {
    case VENDOR: return "x86recomp";
    case RENDERER: return "gfx::Device";
    case VERSION: return "1.1.0";
    case EXTENSIONS:
      return "GL_ARB_multitexture GL_EXT_texture_env_add "
             "GL_ARB_texture_env_combine GL_EXT_texture_env_combine "
             "GL_EXT_compiled_vertex_array GL_ARB_texture_compression "
             "GL_EXT_texture_compression_s3tc GL_EXT_texture_filter_anisotropic "
             "GL_EXT_bgra GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp";
    default: return nullptr;
  }
}

bool Context::GetIntegerv(Enum pname, int32_t* out) {
  switch (pname) {
    // Games also check it against the screen size (a 2560-wide mode needs
    // more than 2048); every SDL_GPU and d3d9 target takes 8192.
    case MAX_TEXTURE_SIZE: *out = 8192; return true;
    case MAX_TEXTURE_UNITS: *out = kUnits; return true;
    case VIEWPORT: std::copy(vp_, vp_ + 4, out); return true;
    case DEPTH_BITS: *out = 24; return true;
    case STENCIL_BITS: *out = 8; return true;
    case RED_BITS: case 0x0D53: case 0x0D54: *out = 8; return true;  // R/G/B
    case ALPHA_BITS: *out = 0; return true;  // X8R8G8B8 back buffer
    case MATRIX_MODE: *out = int32_t(matrix_mode_); return true;
    case MAX_LIGHTS: *out = 8; return true;
    case MAX_CLIP_PLANES: *out = 6; return true;
    default: {
      float f[16];
      if (pname == MAX_TEXTURE_MAX_ANISOTROPY_EXT && GetFloatv(pname, f)) {
        *out = int32_t(f[0]);
        return true;
      }
      Unsupported("glGetIntegerv", pname);
      return false;
    }
  }
}

bool Context::GetFloatv(Enum pname, float* out) {
  switch (pname) {
    case MODELVIEW_MATRIX:
      std::copy_n(stacks_[0].back().data(), 16, out);
      return true;
    case PROJECTION_MATRIX:
      std::copy_n(stacks_[1].back().data(), 16, out);
      return true;
    case MAX_TEXTURE_MAX_ANISOTROPY_EXT: *out = 16; return true;
    default: {
      int32_t v[4];
      if (pname != MAX_TEXTURE_MAX_ANISOTROPY_EXT && GetIntegerv(pname, v)) {
        int n = pname == VIEWPORT ? 4 : 1;
        for (int i = 0; i < n; i++) out[i] = float(v[i]);
        return true;
      }
      return false;
    }
  }
}

// ---- fixed-function state ---------------------------------------------------------

void Context::SetEnabled(Enum cap, bool on) {
  switch (cap) {
    case TEXTURE_2D: units_[active_unit_].tex2d = on; return;
    case CULL_FACE: case FOG: case DEPTH_TEST: case STENCIL_TEST: case ALPHA_TEST:
    case BLEND: case SCISSOR_TEST: case POLYGON_OFFSET_FILL: case DITHER:
    case NORMALIZE: case RESCALE_NORMAL:
      break;
    default:
      if (cap >= CLIP_PLANE0 && cap < CLIP_PLANE0 + 6) break;
      if (cap == LIGHTING || cap == COLOR_MATERIAL || (cap >= LIGHT0 && cap < LIGHT0 + 8))
        break;
      if (cap == TEXTURE_GEN_S || cap == TEXTURE_GEN_T) {
        // Texture coordinate generation is not modelled; turning it on is
        // reported, turning it off is the default anyway.
        if (on) Unsupported("glEnable", cap);
        return;
      }
      Unsupported(on ? "glEnable" : "glDisable", cap);
      return;
  }
  if (on)
    enabled_.insert(cap);
  else
    enabled_.erase(cap);
}

bool Context::IsEnabled(Enum cap) const {
  if (cap == TEXTURE_2D) return units_[active_unit_].tex2d;
  return enabled_.count(cap) != 0;
}

void Context::BlendFunc(Enum s, Enum d) {
  blend_src_ = s;
  blend_dst_ = d;
}

void Context::AlphaFunc(Enum f, float ref) {
  alpha_func_ = f;
  alpha_ref_ = std::clamp(ref, 0.0f, 1.0f);
}

void Context::DepthFunc(Enum f) { depth_func_ = f; }
void Context::DepthMask(bool on) { depth_mask_ = on; }

void Context::DepthRange(double n, double f) {
  depth_near_ = std::clamp(n, 0.0, 1.0);
  depth_far_ = std::clamp(f, 0.0, 1.0);
  matrices_dirty_ = true;  // the device keeps the range in its viewport
}

void Context::ColorMask(bool r, bool g, bool b, bool a) {
  color_mask_ = (r ? 1u : 0) | (g ? 2u : 0) | (b ? 4u : 0) | (a ? 8u : 0);
}

void Context::CullFace(Enum f) { cull_face_ = f; }
void Context::FrontFace(Enum d) { front_face_ = d; }

void Context::PolygonMode(Enum face, Enum mode) {
  if (face != FRONT_AND_BACK) Unsupported("glPolygonMode face", face);
  polygon_mode_ = mode;
}

void Context::PolygonOffset(float factor, float units) {
  offset_factor_ = factor;
  offset_units_ = units;
}

void Context::ShadeModel(Enum m) { shade_model_ = m; }

void Context::Viewport(int x, int y, int w, int h) {
  vp_[0] = x;
  vp_[1] = y;
  vp_[2] = w;
  vp_[3] = h;
  matrices_dirty_ = true;  // the half-pixel offset depends on the size
}

void Context::Scissor(int x, int y, int w, int h) {
  scissor_[0] = x;
  scissor_[1] = y;
  scissor_[2] = w;
  scissor_[3] = h;
}

void Context::StencilFunc(Enum f, int ref, uint32_t mask) {
  stencil_func_ = f;
  stencil_ref_ = ref;
  stencil_mask_ = mask;
}

void Context::StencilOp(Enum fail, Enum zfail, Enum zpass) {
  stencil_fail_ = fail;
  stencil_zfail_ = zfail;
  stencil_zpass_ = zpass;
}

void Context::StencilMask(uint32_t m) { stencil_write_ = m; }

void Context::Fog(Enum pname, const float* v) {
  switch (pname) {
    case FOG_MODE: fog_mode_ = Enum(v[0]); break;
    case FOG_DENSITY: fog_density_ = v[0]; break;
    case FOG_START: fog_start_ = v[0]; break;
    case FOG_END: fog_end_ = v[0]; break;
    case FOG_COLOR: std::copy_n(v, 4, fog_color_); break;
    default: Unsupported("glFog", pname);
  }
}

// Planes are given in object space and kept in eye space, as GL does.
void Context::ClipPlane(Enum plane, const double eqn[4]) {
  int i = int(plane - CLIP_PLANE0);
  if (i < 0 || i >= 6) return Unsupported("glClipPlane", plane);
  // eye = object plane * inverse(modelview): solve by transposed inverse.
  const float* m = stacks_[0].back().data();
  // Inverse of an affine modelview (rotation/scale + translation).
  double a[3][3] = {{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[2], m[6], m[10]}};
  double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
               a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (std::fabs(det) < 1e-12) return Error(INVALID_OPERATION, "glClipPlane");
  double inv[3][3];
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) {
      int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
      inv[r][c] = (a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1]) / det;
    }
  double t[3] = {m[12], m[13], m[14]};
  // p_eye(x) = p_obj(M^-1 x): normal n' = n * A^-1, d' = d - n' . t
  double n[3];
  for (int c = 0; c < 3; c++)
    n[c] = eqn[0] * inv[0][c] + eqn[1] * inv[1][c] + eqn[2] * inv[2][c];
  clip_planes_[i][0] = n[0];
  clip_planes_[i][1] = n[1];
  clip_planes_[i][2] = n[2];
  clip_planes_[i][3] = eqn[3] - (n[0] * t[0] + n[1] * t[1] + n[2] * t[2]);
}

void Context::ClearColor(float r, float g, float b, float a) {
  clear_color_[0] = r;
  clear_color_[1] = g;
  clear_color_[2] = b;
  clear_color_[3] = a;
}
void Context::ClearDepth(double d) { clear_depth_ = std::clamp(d, 0.0, 1.0); }
void Context::ClearStencil(int s) { clear_stencil_ = s; }

void Context::Clear(uint32_t mask) {
  uint32_t flags = 0;
  if ((mask & COLOR_BUFFER_BIT) && color_mask_) flags |= CLEAR_TARGET;
  if ((mask & DEPTH_BUFFER_BIT) && depth_mask_) flags |= CLEAR_ZBUFFER;
  if ((mask & STENCIL_BUFFER_BIT) && stencil_write_) flags |= CLEAR_STENCIL;
  if (!flags) return;
  if (color_mask_ != 0xF && (flags & CLEAR_TARGET))
    Unsupported("glClear with a partial color mask", color_mask_);
  if (!scene_) {
    dev_->BeginScene();
    scene_ = true;
  }
  ApplyState();  // scissor
  // GL clears ignore the viewport; the device's are clipped to it.
  dev_->SetViewport({0, 0, uint32_t(fb_w_), uint32_t(fb_h_), 0, 1});
  gfx::Rect full{0, 0, fb_w_, fb_h_};
  dev_->Clear(1, &full, flags, Argb(clear_color_), float(clear_depth_),
              uint32_t(clear_stencil_));
  matrices_dirty_ = true;  // restores the viewport
}

void Context::PixelStore(Enum pname, int v) {
  switch (pname) {
    case UNPACK_ALIGNMENT: unpack_alignment_ = v; break;
    case PACK_ALIGNMENT: pack_alignment_ = v; break;
    case UNPACK_ROW_LENGTH: unpack_row_length_ = v; break;
    default: Unsupported("glPixelStorei", pname);
  }
}

// ---- lighting ---------------------------------------------------------------------

void Context::Light(Enum light, Enum pname, const float* v) {
  int i = int(light - LIGHT0);
  if (i < 0 || i >= 8) return Error(INVALID_ENUM, "glLight");
  LightState& L = lights_[i];
  const float* mv = stacks_[0].back().data();
  switch (pname) {
    case AMBIENT: std::copy_n(v, 4, L.ambient); break;
    case DIFFUSE: std::copy_n(v, 4, L.diffuse); break;
    case SPECULAR: std::copy_n(v, 4, L.specular); break;
    case POSITION:  // stored in eye space: transformed by the modelview now
      for (int r = 0; r < 4; r++)
        L.position[r] = mv[r] * v[0] + mv[4 + r] * v[1] + mv[8 + r] * v[2] + mv[12 + r] * v[3];
      break;
    case SPOT_DIRECTION:
      for (int r = 0; r < 3; r++)
        L.spot_dir[r] = mv[r] * v[0] + mv[4 + r] * v[1] + mv[8 + r] * v[2];
      break;
    case SPOT_EXPONENT: L.spot_exp = v[0]; break;
    case SPOT_CUTOFF: L.spot_cutoff = v[0]; break;
    case CONSTANT_ATTENUATION: L.att[0] = v[0]; break;
    case LINEAR_ATTENUATION: L.att[1] = v[0]; break;
    case QUADRATIC_ATTENUATION: L.att[2] = v[0]; break;
    default: Unsupported("glLight", pname);
  }
}

void Context::LightModel(Enum pname, const float* v) {
  switch (pname) {
    case LIGHT_MODEL_AMBIENT: std::copy_n(v, 4, model_ambient_); break;
    case LIGHT_MODEL_LOCAL_VIEWER:
    case LIGHT_MODEL_TWO_SIDE:
      if (v[0] != 0) Unsupported("glLightModel", pname);
      break;
    default: Unsupported("glLightModel", pname);
  }
}

void Context::Material(Enum face, Enum pname, const float* v) {
  if (face == BACK) return;  // one-sided lighting: back materials unused
  switch (pname) {
    case AMBIENT: std::copy_n(v, 4, mat_ambient_); break;
    case DIFFUSE: std::copy_n(v, 4, mat_diffuse_); break;
    case AMBIENT_AND_DIFFUSE:
      std::copy_n(v, 4, mat_ambient_);
      std::copy_n(v, 4, mat_diffuse_);
      break;
    case SPECULAR: std::copy_n(v, 4, mat_specular_); break;
    case EMISSION: std::copy_n(v, 4, mat_emission_); break;
    case SHININESS: mat_shininess_ = v[0]; break;
    default: Unsupported("glMaterial", pname);
  }
}

void Context::ColorMaterial(Enum face, Enum mode) {
  if (face == BACK) return Unsupported("glColorMaterial face", face);
  color_material_ = mode;
}

// ---- matrices --------------------------------------------------------------------

float* Context::Top() {
  int i = matrix_mode_ == MODELVIEW ? 0 : matrix_mode_ == PROJECTION ? 1 : 2 + active_unit_;
  return stacks_[i].back().data();
}

void Context::MarkMatrix() { matrices_dirty_ = true; }

void Context::MatrixMode(Enum mode) {
  if (mode != MODELVIEW && mode != PROJECTION && mode != TEXTURE)
    return Unsupported("glMatrixMode", mode);
  matrix_mode_ = mode;
}

void Context::LoadIdentity() {
  auto id = Identity();
  std::copy(id.begin(), id.end(), Top());
  MarkMatrix();
}

void Context::LoadMatrix(const float m[16]) {
  std::copy_n(m, 16, Top());
  MarkMatrix();
}

void Context::MultMatrix(const float m[16]) {
  auto r = Mul(Top(), m);
  std::copy(r.begin(), r.end(), Top());
  MarkMatrix();
}

void Context::PushMatrix() {
  int i = matrix_mode_ == MODELVIEW ? 0 : matrix_mode_ == PROJECTION ? 1 : 2 + active_unit_;
  size_t max = i == 0 ? 32 : 2;
  if (stacks_[i].size() >= max) return Error(STACK_OVERFLOW, "glPushMatrix");
  stacks_[i].push_back(stacks_[i].back());
}

void Context::PopMatrix() {
  int i = matrix_mode_ == MODELVIEW ? 0 : matrix_mode_ == PROJECTION ? 1 : 2 + active_unit_;
  if (stacks_[i].size() <= 1) return Error(STACK_UNDERFLOW, "glPopMatrix");
  stacks_[i].pop_back();
  MarkMatrix();
}

void Context::Ortho(double l, double r, double b, double t, double n, double f) {
  float m[16] = {float(2 / (r - l)), 0, 0, 0,
                 0, float(2 / (t - b)), 0, 0,
                 0, 0, float(-2 / (f - n)), 0,
                 float(-(r + l) / (r - l)), float(-(t + b) / (t - b)),
                 float(-(f + n) / (f - n)), 1};
  MultMatrix(m);
}

void Context::Frustum(double l, double r, double b, double t, double n, double f) {
  float m[16] = {float(2 * n / (r - l)), 0, 0, 0,
                 0, float(2 * n / (t - b)), 0, 0,
                 float((r + l) / (r - l)), float((t + b) / (t - b)),
                 float(-(f + n) / (f - n)), -1,
                 0, 0, float(-2 * f * n / (f - n)), 0};
  MultMatrix(m);
}

void Context::Translate(float x, float y, float z) {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
  MultMatrix(m);
}

void Context::Scale(float x, float y, float z) {
  float m[16] = {x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1};
  MultMatrix(m);
}

void Context::Rotate(float angle, float x, float y, float z) {
  double len = std::sqrt(double(x) * x + double(y) * y + double(z) * z);
  if (len == 0) return;
  double a = angle * std::numbers::pi / 180, c = std::cos(a), s = std::sin(a);
  double ux = x / len, uy = y / len, uz = z / len;
  float m[16] = {float(ux * ux * (1 - c) + c), float(uy * ux * (1 - c) + uz * s),
                 float(ux * uz * (1 - c) - uy * s), 0,
                 float(ux * uy * (1 - c) - uz * s), float(uy * uy * (1 - c) + c),
                 float(uy * uz * (1 - c) + ux * s), 0,
                 float(ux * uz * (1 - c) + uy * s), float(uy * uz * (1 - c) - ux * s),
                 float(uz * uz * (1 - c) + c), 0,
                 0, 0, 0, 1};
  MultMatrix(m);
}

void Context::ApplyMatrices() {
  if (!matrices_dirty_) return;
  matrices_dirty_ = false;
  dev_->SetTransform(TS_WORLD, stacks_[0].back().data());
  // Projection: z from -w..w to 0..w, and the half-pixel shift.
  std::array<float, 16> p = stacks_[1].back();
  float vw = float(std::max(vp_[2], 1)), vh = float(std::max(vp_[3], 1));
  for (int k = 0; k < 4; k++) {
    float w = p[k * 4 + 3];
    p[k * 4 + 2] = 0.5f * p[k * 4 + 2] + 0.5f * w;
    p[k * 4 + 0] -= w / vw;
    p[k * 4 + 1] += w / vh;
  }
  dev_->SetTransform(TS_PROJECTION, p.data());
  for (int u = 0; u < kUnits; u++) {
    const auto& m = stacks_[2 + u].back();
    if (IsIdentity(m)) {
      TSS(u, TSS_TEXTURETRANSFORMFLAGS, 0);
      continue;
    }
    // GL: (s,t,0,1) * columns; device: (u,v,1,0) * rows.
    float d[16];
    for (int j = 0; j < 4; j++) {
      d[0 + j] = m[j];
      d[4 + j] = m[4 + j];
      d[8 + j] = m[12 + j];
      d[12 + j] = m[8 + j];
    }
    dev_->SetTransform(TS_TEXTURE0 + uint32_t(u), d);
    TSS(u, TSS_TEXTURETRANSFORMFLAGS, 2);  // D3DTTFF_COUNT2
  }
  // Viewport, flipped to a top-left origin and clipped to the target.
  int x = std::clamp(vp_[0], 0, fb_w_), y = fb_h_ - (vp_[1] + vp_[3]);
  int y0 = std::clamp(y, 0, fb_h_), x1 = std::clamp(vp_[0] + vp_[2], 0, fb_w_);
  int y1 = std::clamp(y + vp_[3], 0, fb_h_);
  if (x != vp_[0] || y0 != y || x1 != vp_[0] + vp_[2] || y1 != y + vp_[3])
    Unsupported("viewport outside the framebuffer", 0);
  dev_->SetViewport({uint32_t(x), uint32_t(y0), uint32_t(std::max(x1 - x, 0)),
                     uint32_t(std::max(y1 - y0, 0)), float(depth_near_),
                     float(depth_far_)});
}

// ---- device state ------------------------------------------------------------------

void Context::RS(uint32_t s, uint32_t v) {
  if (rs_cache_[s] == v) return;
  rs_cache_[s] = v;
  dev_->SetRenderState(s, v);
}

void Context::TSS(uint32_t stage, uint32_t type, uint32_t v) {
  if (tss_cache_[stage][type] == v) return;
  tss_cache_[stage][type] = v;
  dev_->SetTextureStageState(stage, type, v);
}

void Context::ApplyState() {
  bool depth = enabled_.count(DEPTH_TEST);
  RS(RS_ZENABLE, depth ? 1 : 0);
  RS(RS_ZWRITEENABLE, depth && depth_mask_ ? 1 : 0);
  RS(RS_ZFUNC, CmpFunc(depth_func_));
  RS(RS_ALPHABLENDENABLE, enabled_.count(BLEND) ? 1 : 0);
  RS(RS_SRCBLEND, BlendFactor(blend_src_));
  RS(RS_DESTBLEND, BlendFactor(blend_dst_));
  RS(RS_ALPHATESTENABLE, enabled_.count(ALPHA_TEST) ? 1 : 0);
  RS(RS_ALPHAFUNC, CmpFunc(alpha_func_));
  RS(RS_ALPHAREF, uint32_t(std::lround(alpha_ref_ * 255)));
  uint32_t cull = 1;  // D3DCULL_NONE
  if (enabled_.count(CULL_FACE)) {
    bool cull_back = cull_face_ == BACK;
    if (cull_face_ == FRONT_AND_BACK) Unsupported("glCullFace", cull_face_);
    if (front_face_ == CW) cull_back = !cull_back;
    cull = cull_back ? 2 : 3;  // CW : CCW (see the file comment)
  }
  RS(RS_CULLMODE, cull);
  RS(RS_FILLMODE, polygon_mode_ == POINT ? 1 : polygon_mode_ == LINE ? 2 : 3);
  RS(RS_SHADEMODE, shade_model_ == FLAT ? 1 : 2);
  bool offset = enabled_.count(POLYGON_OFFSET_FILL);
  // GL units are steps of the 24-bit depth buffer; the device bias is in
  // depth (0..1) units.
  RS(RS_SLOPESCALEDEPTHBIAS, FloatBits(offset ? offset_factor_ : 0.0f));
  RS(RS_DEPTHBIAS, FloatBits(offset ? offset_units_ / 16777216.0f : 0.0f));
  RS(RS_COLORWRITEENABLE, color_mask_);
  bool scissor = enabled_.count(SCISSOR_TEST);
  RS(RS_SCISSORTESTENABLE, scissor ? 1 : 0);
  if (scissor) {
    int x0 = std::clamp(scissor_[0], 0, fb_w_);
    int x1 = std::clamp(scissor_[0] + scissor_[2], 0, fb_w_);
    int y0 = std::clamp(fb_h_ - (scissor_[1] + scissor_[3]), 0, fb_h_);
    int y1 = std::clamp(fb_h_ - scissor_[1], 0, fb_h_);
    dev_->SetScissor({x0, y0, x1, y1});
  }
  bool stencil = enabled_.count(STENCIL_TEST);
  RS(RS_STENCILENABLE, stencil ? 1 : 0);
  if (stencil) {
    RS(RS_STENCILFUNC, CmpFunc(stencil_func_));
    RS(RS_STENCILREF, uint32_t(stencil_ref_));
    RS(RS_STENCILMASK, stencil_mask_);
    RS(RS_STENCILWRITEMASK, stencil_write_);
    RS(RS_STENCILFAIL, StencilOpOf(stencil_fail_));
    RS(RS_STENCILZFAIL, StencilOpOf(stencil_zfail_));
    RS(RS_STENCILPASS, StencilOpOf(stencil_zpass_));
  }
  bool fog = enabled_.count(FOG);
  RS(RS_FOGENABLE, fog ? 1 : 0);
  if (fog) {
    // Per-vertex fog on eye-space z, as GL computes it.
    RS(RS_FOGTABLEMODE, 0);
    RS(RS_FOGVERTEXMODE, fog_mode_ == LINEAR ? 3 : fog_mode_ == EXP2 ? 2 : 1);
    RS(RS_FOGCOLOR, Argb(fog_color_) & 0x00FFFFFF);
    RS(RS_FOGSTART, FloatBits(fog_start_));
    RS(RS_FOGEND, FloatBits(fog_end_));
    RS(RS_FOGDENSITY, FloatBits(fog_density_));
  }
  bool lighting = enabled_.count(LIGHTING);
  RS(RS_LIGHTING, lighting ? 1 : 0);
  RS(RS_SPECULARENABLE, 0);
  if (lighting) {
    RS(RS_LOCALVIEWER, 0);  // GL's default LIGHT_MODEL_LOCAL_VIEWER
    RS(RS_NORMALIZENORMALS, enabled_.count(NORMALIZE) || enabled_.count(RESCALE_NORMAL));
    RS(RS_AMBIENT, Argb(model_ambient_));
    bool cm = enabled_.count(COLOR_MATERIAL);
    RS(RS_COLORVERTEX, cm ? 1 : 0);
    auto src = [&](Enum m) { return cm && (color_material_ == m ||
                                           (color_material_ == AMBIENT_AND_DIFFUSE &&
                                            (m == AMBIENT || m == DIFFUSE))) ? 1u : 0u; };
    RS(RS_DIFFUSEMATERIALSOURCE, src(DIFFUSE));
    RS(RS_AMBIENTMATERIALSOURCE, src(AMBIENT));
    RS(RS_SPECULARMATERIALSOURCE, src(SPECULAR));
    RS(RS_EMISSIVEMATERIALSOURCE, src(EMISSION));
    auto col = [](const float* f) { return gfx::Color{f[0], f[1], f[2], f[3]}; };
    dev_->SetMaterial({col(mat_diffuse_), col(mat_ambient_), col(mat_specular_),
                       col(mat_emission_), mat_shininess_});
    for (int i = 0; i < 8; i++) {
      bool on = enabled_.count(LIGHT0 + Enum(i));
      dev_->LightEnable(uint32_t(i), on);
      if (!on) continue;
      const LightState& L = lights_[i];
      gfx::Light d{};
      d.diffuse = col(L.diffuse);
      d.specular = col(L.specular);
      d.ambient = col(L.ambient);
      d.attenuation0 = L.att[0];
      d.attenuation1 = L.att[1];
      d.attenuation2 = L.att[2];
      d.range = 1e15f;  // GL lights reach everywhere
      if (L.position[3] == 0) {
        d.type = 3;  // directional: travels away from the given position
        for (int k = 0; k < 3; k++) d.direction[k] = -L.position[k];
      } else {
        for (int k = 0; k < 3; k++) d.position[k] = L.position[k] / L.position[3];
        d.type = 1;  // point
        if (L.spot_cutoff != 180) {
          // GL's cos^exponent falloff has no exact device equivalent.
          Unsupported("spot light", Enum(i));
          d.type = 2;
          std::copy_n(L.spot_dir, 3, d.direction);
          d.phi = 2 * L.spot_cutoff * float(std::numbers::pi) / 180;
          d.theta = 0;
          d.falloff = 1;
        }
      }
      dev_->SetLight(uint32_t(i), d);
    }
  }
  uint32_t planes = 0;
  for (int i = 0; i < 6; i++)
    if (enabled_.count(CLIP_PLANE0 + Enum(i))) {
      // The device's world space is GL's eye space (see the file comment).
      float p[4] = {float(clip_planes_[i][0]), float(clip_planes_[i][1]),
                    float(clip_planes_[i][2]), float(clip_planes_[i][3])};
      dev_->SetClipPlane(uint32_t(i), p);
      planes |= 1u << i;
    }
  RS(RS_CLIPPLANEENABLE, planes);
}

Context::Texture* Context::Bound(int unit) {
  auto it = textures_.find(units_[unit].bound);
  return it == textures_.end() ? nullptr : it->second;
}

int Context::ApplyTextures() {
  int last = -1;
  bool live[kUnits] = {};
  for (int u = 0; u < kUnits; u++) {
    Texture* t = Bound(u);
    live[u] = units_[u].tex2d && t && t->Complete();
    if (units_[u].tex2d && t && !t->Complete() && t->id)
      Unsupported("incomplete texture (mipmaps missing) on unit", Enum(u));
    if (live[u]) last = u;
  }
  uint32_t tfactor = kUnset;
  for (int u = 0; u <= last; u++) {
    Unit& un = units_[u];
    TSS(u, TSS_TEXCOORDINDEX, uint32_t(u));
    if (!live[u]) {  // a disabled unit between enabled ones passes through
      dev_->SetTexture(uint32_t(u), 0);
      TSS(u, TSS_COLOROP, TOP_SELECTARG1);
      TSS(u, TSS_COLORARG1, TA_CURRENT);
      TSS(u, TSS_ALPHAOP, TOP_SELECTARG1);
      TSS(u, TSS_ALPHAARG1, TA_CURRENT);
      continue;
    }
    Texture* t = Bound(u);
    dev_->SetTexture(uint32_t(u), t->id);
    // Sampler state from the texture object.
    bool aniso = t->aniso > 1;
    TSS(u, TSS_MAGFILTER, t->mag_filter == NEAREST ? 1 : aniso ? 5 : 2);
    bool min_linear = t->min_filter == LINEAR || t->min_filter == LINEAR_MIPMAP_NEAREST ||
                      t->min_filter == LINEAR_MIPMAP_LINEAR;
    TSS(u, TSS_MINFILTER, !min_linear ? 1 : aniso ? 3 : 2);
    TSS(u, TSS_MIPFILTER, !UsesMipmaps(t->min_filter) ? 1
                          : (t->min_filter == NEAREST_MIPMAP_LINEAR ||
                             t->min_filter == LINEAR_MIPMAP_LINEAR) ? 3 : 2);
    TSS(u, TSS_MAXANISOTROPY, uint32_t(std::max(1.0f, t->aniso)));
    TSS(u, TSS_ADDRESSU, t->wrap_s == REPEAT ? 1 : 3);
    TSS(u, TSS_ADDRESSV, t->wrap_t == REPEAT ? 1 : 3);
    // Environment.
    uint32_t cop = TOP_SELECTARG1, ca1 = TA_CURRENT, ca2 = TA_CURRENT, ca0 = TA_CURRENT;
    uint32_t aop = TOP_SELECTARG1, aa1 = TA_CURRENT, aa2 = TA_CURRENT, aa0 = TA_CURRENT;
    switch (un.env_mode) {
      case MODULATE:
        if (t->has_color) cop = TOP_MODULATE, ca1 = TA_TEXTURE;
        if (t->has_alpha) aop = TOP_MODULATE, aa1 = TA_TEXTURE;
        break;
      case REPLACE:
        if (t->has_color) ca1 = TA_TEXTURE;
        if (t->has_alpha) aa1 = TA_TEXTURE;
        break;
      case DECAL:
        if (t->has_alpha)
          cop = TOP_BLENDTEXTUREALPHA, ca1 = TA_TEXTURE;
        else
          ca1 = TA_TEXTURE;
        break;
      case ADD:
        if (t->has_color) cop = TOP_ADD, ca1 = TA_TEXTURE;
        if (t->has_alpha) aop = TOP_MODULATE, aa1 = TA_TEXTURE;
        break;
      case BLEND_MODE:
        // Cf * (1 - Ct) + Cc * Ct: LERP(Ct, Cc, Cf).
        cop = TOP_LERP, ca0 = TA_TEXTURE, ca1 = TA_TFACTOR, ca2 = TA_CURRENT;
        if (t->has_alpha) aop = TOP_MODULATE, aa1 = TA_TEXTURE;
        tfactor = Argb(un.env_color);
        break;
      case COMBINE: {
        auto src = [&](Enum s, Enum op, bool alpha) -> uint32_t {
          uint32_t a = s == TEXTURE ? TA_TEXTURE : s == PREVIOUS ? TA_CURRENT
                     : s == PRIMARY_COLOR ? TA_DIFFUSE : TA_TFACTOR;
          if (s == CONSTANT) tfactor = Argb(un.env_color);
          if (op == ONE_MINUS_SRC_COLOR || op == ONE_MINUS_SRC_ALPHA) a |= TA_COMPLEMENT;
          if (!alpha && (op == SRC_ALPHA || op == ONE_MINUS_SRC_ALPHA)) a |= TA_ALPHAREPLICATE;
          return a;
        };
        auto op_of = [&](Enum f, float scale, const Enum* s, const Enum* o, bool alpha,
                         uint32_t* top, uint32_t* a0, uint32_t* a1, uint32_t* a2) {
          *a1 = src(s[0], o[0], alpha);
          *a2 = src(s[1], o[1], alpha);
          switch (f) {
            case REPLACE: *top = TOP_SELECTARG1; break;
            case MODULATE:
              *top = scale == 4 ? TOP_MODULATE4X : scale == 2 ? TOP_MODULATE2X : TOP_MODULATE;
              scale = 1;
              break;
            case ADD: *top = TOP_ADD; break;
            case ADD_SIGNED:
              *top = scale == 2 ? TOP_ADDSIGNED2X : TOP_ADDSIGNED;
              if (scale == 2) scale = 1;
              break;
            case SUBTRACT: *top = TOP_SUBTRACT; break;
            case INTERPOLATE:
              // s0 * s2 + s1 * (1 - s2): LERP(s2, s0, s1)
              *top = TOP_LERP;
              *a0 = src(s[2], o[2], alpha);
              break;
            case DOT3_RGB: case DOT3_RGBA: *top = TOP_DOTPRODUCT3; break;
            default: Unsupported("GL_COMBINE function", f); break;
          }
          if (scale != 1) Unsupported("GL_COMBINE scale with this function", f);
        };
        op_of(un.combine_rgb, un.rgb_scale, un.src_rgb, un.op_rgb, false, &cop, &ca0, &ca1, &ca2);
        if (un.combine_rgb == DOT3_RGBA) {
          aop = TOP_DOTPRODUCT3, aa1 = ca1, aa2 = ca2;
        } else {
          op_of(un.combine_alpha, un.alpha_scale, un.src_alpha, un.op_alpha, true, &aop,
                &aa0, &aa1, &aa2);
        }
        break;
      }
      default:
        Unsupported("GL_TEXTURE_ENV_MODE", un.env_mode);
        cop = TOP_MODULATE, ca1 = TA_TEXTURE, aop = TOP_MODULATE, aa1 = TA_TEXTURE;
        break;
    }
    TSS(u, TSS_COLOROP, cop);
    TSS(u, TSS_COLORARG1, ca1);
    TSS(u, TSS_COLORARG2, ca2);
    TSS(u, TSS_COLORARG0, ca0);
    TSS(u, TSS_ALPHAOP, aop);
    TSS(u, TSS_ALPHAARG1, aa1);
    TSS(u, TSS_ALPHAARG2, aa2);
    TSS(u, TSS_ALPHAARG0, aa0);
  }
  if (tfactor != kUnset) RS(RS_TEXTUREFACTOR, tfactor);
  // End the chain; with no texture at all, stage 0 passes the vertex color.
  int end = last + 1;
  if (end == 0) {
    dev_->SetTexture(0, 0);
    TSS(0, TSS_COLOROP, TOP_SELECTARG1);
    TSS(0, TSS_COLORARG1, TA_DIFFUSE);
    TSS(0, TSS_ALPHAOP, TOP_SELECTARG1);
    TSS(0, TSS_ALPHAARG1, TA_DIFFUSE);
    end = 1;
  }
  if (end < 8) {
    TSS(end, TSS_COLOROP, TOP_DISABLE);
    TSS(end, TSS_ALPHAOP, TOP_DISABLE);
  }
  return last + 1;
}

// ---- textures --------------------------------------------------------------------

void Context::GenTextures(int n, uint32_t* names) {
  for (int i = 0; i < n; i++) {
    while (textures_.count(next_name_)) next_name_++;
    names[i] = next_name_;
    textures_[next_name_++] = new Texture;
  }
}

void Context::DeleteTextures(int n, const uint32_t* names) {
  for (int i = 0; i < n; i++) {
    uint32_t name = names[i];
    if (!name) continue;
    auto it = textures_.find(name);
    if (it == textures_.end()) continue;
    if (it->second->id) dev_->DestroyTexture(it->second->id);
    delete it->second;
    textures_.erase(it);
    for (int u = 0; u < kUnits; u++)
      if (units_[u].bound == name) units_[u].bound = 0;
  }
}

void Context::BindTexture(Enum target, uint32_t name) {
  if (target != TEXTURE_2D) return Unsupported("glBindTexture target", target);
  if (!textures_.count(name)) textures_[name] = new Texture;  // GL creates on bind
  units_[active_unit_].bound = name;
}

void Context::TexParameter(Enum target, Enum pname, const float* v) {
  if (target != TEXTURE_2D) return Unsupported("glTexParameter target", target);
  Texture* t = Bound(active_unit_);
  Enum e = Enum(v[0]);
  switch (pname) {
    case TEXTURE_MIN_FILTER: t->min_filter = e; break;
    case TEXTURE_MAG_FILTER: t->mag_filter = e; break;
    case TEXTURE_WRAP_S: t->wrap_s = e; break;
    case TEXTURE_WRAP_T: t->wrap_t = e; break;
    case TEXTURE_MAX_ANISOTROPY_EXT: t->aniso = std::clamp(v[0], 1.0f, 16.0f); break;
    default: Unsupported("glTexParameter", pname);
  }
}

namespace {

struct BaseFormat {
  bool ok, has_color, has_alpha;
};

BaseFormat BaseOf(Enum internal) {
  switch (internal) {
    case 3: case RGB: case RGB4: case RGB5: case RGB8: return {true, true, false};
    case 4: case RGBA: case RGBA4: case RGB5_A1: case RGBA8: return {true, true, true};
    case 1: case LUMINANCE: case LUMINANCE8: return {true, true, false};
    case 2: case LUMINANCE_ALPHA: case LUMINANCE8_ALPHA8: return {true, true, true};
    case ALPHA: case ALPHA8: return {true, false, true};
    case INTENSITY: case INTENSITY8: return {true, true, true};
    default: return {false, false, false};
  }
}

// Converts w x h pixels of `format` (GL_UNSIGNED_BYTE) into A8R8G8B8 as
// the base format sees them; false if the format is not handled.
bool ToArgb(Enum internal, Enum format, int w, int h, int row_length, int alignment,
            const uint8_t* src, std::vector<uint8_t>* out) {
  int comps;
  switch (format) {
    case RGBA: case BGRA_EXT: comps = 4; break;
    case RGB: case BGR_EXT: comps = 3; break;
    case LUMINANCE_ALPHA: comps = 2; break;
    case LUMINANCE: case ALPHA: comps = 1; break;
    default: return false;
  }
  int row_pixels = row_length > 0 ? row_length : w;
  size_t stride = size_t(row_pixels) * size_t(comps);
  stride = (stride + size_t(alignment) - 1) / size_t(alignment) * size_t(alignment);
  BaseFormat base = BaseOf(internal);
  bool intensity = internal == INTENSITY || internal == INTENSITY8;
  out->resize(size_t(w) * size_t(h) * 4);
  for (int y = 0; y < h; y++) {
    const uint8_t* s = src + size_t(y) * stride;
    uint8_t* d = out->data() + size_t(y) * size_t(w) * 4;
    for (int x = 0; x < w; x++, s += comps, d += 4) {
      uint8_t r, g, b, a;
      switch (format) {
        case RGBA: r = s[0]; g = s[1]; b = s[2]; a = s[3]; break;
        case BGRA_EXT: b = s[0]; g = s[1]; r = s[2]; a = s[3]; break;
        case RGB: r = s[0]; g = s[1]; b = s[2]; a = 255; break;
        case BGR_EXT: b = s[0]; g = s[1]; r = s[2]; a = 255; break;
        case LUMINANCE_ALPHA: r = g = b = s[0]; a = s[1]; break;
        case LUMINANCE: r = g = b = s[0]; a = 255; break;
        default: r = g = b = 0; a = s[0]; break;  // ALPHA
      }
      // What the internal (base) format keeps of it.
      if (intensity) {
        r = g = b = a = r;
      } else if (!base.has_color) {
        r = g = b = 255;  // only alpha is stored; colour reads as 1
      } else if (internal == 1 || internal == LUMINANCE || internal == LUMINANCE8 ||
                 internal == 2 || internal == LUMINANCE_ALPHA ||
                 internal == LUMINANCE8_ALPHA8) {
        g = b = r;
      }
      if (!base.has_alpha) a = 255;
      d[0] = b;
      d[1] = g;
      d[2] = r;
      d[3] = a;
    }
  }
  return true;
}

}  // namespace

void Context::TexImage2D(Enum target, int level, Enum internal, int w, int h, int border,
                         Enum format, Enum type, const void* pixels) {
  if (target != TEXTURE_2D) return Unsupported("glTexImage2D target", target);
  if (border) return Unsupported("glTexImage2D border", Enum(border));
  if (type != UNSIGNED_BYTE) return Unsupported("glTexImage2D type", type);
  BaseFormat base = BaseOf(internal);
  if (!base.ok) return Unsupported("glTexImage2D internal format", internal);
  Texture* t = Bound(active_unit_);
  if (level == 0 && (t->w != w || t->h != h || t->compressed || !t->id)) {
    if (t->id) dev_->DestroyTexture(t->id);
    t->w = w;
    t->h = h;
    t->levels = MaxLevels(w, h);
    t->compressed = false;
    t->fmt = gfx::Format::A8R8G8B8;
    t->id = dev_->CreateTexture(w, h, t->levels, t->fmt, false);
    t->defined.assign(size_t(t->levels), false);
  }
  if (level == 0) {
    t->has_color = base.has_color;
    t->has_alpha = base.has_alpha;
  }
  if (!t->id || level >= t->levels || t->compressed ||
      w != std::max(1, t->w >> level) || h != std::max(1, t->h >> level))
    return Error(INVALID_VALUE, "glTexImage2D level size");
  t->defined[size_t(level)] = true;
  if (!pixels) return;
  std::vector<uint8_t> argb;
  if (!ToArgb(internal, format, w, h, unpack_row_length_, unpack_alignment_,
              static_cast<const uint8_t*>(pixels), &argb))
    return Unsupported("glTexImage2D format", format);
  dev_->UpdateTexture(t->id, level, {0, 0, w, h}, argb.data(), uint32_t(w) * 4);
}

void Context::TexSubImage2D(Enum target, int level, int x, int y, int w, int h,
                            Enum format, Enum type, const void* pixels) {
  if (target != TEXTURE_2D) return Unsupported("glTexSubImage2D target", target);
  if (type != UNSIGNED_BYTE) return Unsupported("glTexSubImage2D type", type);
  Texture* t = Bound(active_unit_);
  if (!t->id || t->compressed || level >= t->levels || !t->defined[size_t(level)])
    return Error(INVALID_OPERATION, "glTexSubImage2D");
  // The base format of the existing image decides what is kept.
  Enum internal = t->has_color ? (t->has_alpha ? RGBA : RGB) : ALPHA;
  std::vector<uint8_t> argb;
  if (!ToArgb(internal, format, w, h, unpack_row_length_, unpack_alignment_,
              static_cast<const uint8_t*>(pixels), &argb))
    return Unsupported("glTexSubImage2D format", format);
  dev_->UpdateTexture(t->id, level, {x, y, x + w, y + h}, argb.data(), uint32_t(w) * 4);
}

void Context::CompressedTexImage2D(Enum target, int level, Enum internal, int w, int h,
                                   int border, int size, const void* data) {
  if (target != TEXTURE_2D || border) return Unsupported("glCompressedTexImage2D", target);
  gfx::Format fmt;
  bool alpha = true;
  switch (internal) {
    case COMPRESSED_RGB_S3TC_DXT1: fmt = gfx::Format::DXT1; alpha = false; break;
    case COMPRESSED_RGBA_S3TC_DXT1: fmt = gfx::Format::DXT1; break;
    case COMPRESSED_RGBA_S3TC_DXT3: fmt = gfx::Format::DXT3; break;
    case COMPRESSED_RGBA_S3TC_DXT5: fmt = gfx::Format::DXT5; break;
    default: return Unsupported("glCompressedTexImage2D format", internal);
  }
  Texture* t = Bound(active_unit_);
  if (level == 0 && (t->w != w || t->h != h || t->fmt != fmt || !t->id)) {
    if (t->id) dev_->DestroyTexture(t->id);
    t->w = w;
    t->h = h;
    t->levels = MaxLevels(w, h);
    t->compressed = true;
    t->fmt = fmt;
    t->id = dev_->CreateTexture(w, h, t->levels, fmt, false);
    t->defined.assign(size_t(t->levels), false);
    t->has_color = true;
    t->has_alpha = alpha;
  }
  if (!t->id || level >= t->levels || t->fmt != fmt ||
      w != std::max(1, t->w >> level) || h != std::max(1, t->h >> level))
    return Error(INVALID_VALUE, "glCompressedTexImage2D level size");
  uint32_t pitch = gfx::RowPitch(fmt, uint32_t(w));
  if (uint64_t(pitch) * gfx::RowCount(fmt, uint32_t(h)) > uint64_t(size))
    return Error(INVALID_VALUE, "glCompressedTexImage2D size");
  t->defined[size_t(level)] = true;
  dev_->UpdateTexture(t->id, level, {0, 0, w, h}, data, pitch);
}

void Context::TexEnv(Enum target, Enum pname, const float* v) {
  if (target != TEXTURE_ENV) return Unsupported("glTexEnv target", target);
  Unit& u = units_[active_unit_];
  Enum e = Enum(v[0]);
  switch (pname) {
    case TEXTURE_ENV_MODE: u.env_mode = e; break;
    case TEXTURE_ENV_COLOR: std::copy_n(v, 4, u.env_color); break;
    case COMBINE_RGB: u.combine_rgb = e; break;
    case COMBINE_ALPHA: u.combine_alpha = e; break;
    case RGB_SCALE: u.rgb_scale = v[0]; break;
    case ALPHA_SCALE: u.alpha_scale = v[0]; break;
    default:
      if (pname >= SOURCE0_RGB && pname <= SOURCE2_RGB) u.src_rgb[pname - SOURCE0_RGB] = e;
      else if (pname >= SOURCE0_ALPHA && pname <= SOURCE2_ALPHA)
        u.src_alpha[pname - SOURCE0_ALPHA] = e;
      else if (pname >= OPERAND0_RGB && pname <= OPERAND2_RGB)
        u.op_rgb[pname - OPERAND0_RGB] = e;
      else if (pname >= OPERAND0_ALPHA && pname <= OPERAND2_ALPHA)
        u.op_alpha[pname - OPERAND0_ALPHA] = e;
      else
        Unsupported("glTexEnv", pname);
  }
}

void Context::ActiveTexture(Enum unit) {
  int u = int(unit - TEXTURE0);
  if (u < 0 || u >= kUnits) return Error(INVALID_ENUM, "glActiveTextureARB");
  active_unit_ = u;
}

void Context::ClientActiveTexture(Enum unit) {
  int u = int(unit - TEXTURE0);
  if (u < 0 || u >= kUnits) return Error(INVALID_ENUM, "glClientActiveTextureARB");
  client_unit_ = u;
}

// ---- vertex arrays ------------------------------------------------------------------

void Context::VertexPointer(int size, Enum type, int stride, const void* p) {
  vertex_.size = size;
  vertex_.type = type;
  vertex_.stride = stride;
  vertex_.ptr = static_cast<const uint8_t*>(p);
}

void Context::ColorPointer(int size, Enum type, int stride, const void* p) {
  color_.size = size;
  color_.type = type;
  color_.stride = stride;
  color_.ptr = static_cast<const uint8_t*>(p);
}

void Context::TexCoordPointer(int size, Enum type, int stride, const void* p) {
  Array& a = texcoord_[client_unit_];
  a.size = size;
  a.type = type;
  a.stride = stride;
  a.ptr = static_cast<const uint8_t*>(p);
}

void Context::NormalPointer(Enum type, int stride, const void* p) {
  normal_.size = 3;
  normal_.type = type;
  normal_.stride = stride;
  normal_.ptr = static_cast<const uint8_t*>(p);
}

void Context::SetClientState(Enum array, bool on) {
  switch (array) {
    case VERTEX_ARRAY: vertex_.enabled = on; break;
    case COLOR_ARRAY: color_.enabled = on; break;
    case NORMAL_ARRAY: normal_.enabled = on; break;
    case TEXTURE_COORD_ARRAY: texcoord_[client_unit_].enabled = on; break;
    default: Unsupported("glEnableClientState", array);
  }
}

void Context::FetchVertex(int i, Vtx* v) const {
  *v = current_;
  auto at = [&](const Array& a) {
    int stride = a.stride ? a.stride : a.size * TypeSize(a.type);
    return a.ptr + size_t(i) * size_t(stride);
  };
  if (vertex_.enabled && vertex_.ptr) {
    const uint8_t* p = at(vertex_);
    int cs = TypeSize(vertex_.type);
    v->pos[0] = ReadComponent(p, vertex_.type, false);
    v->pos[1] = vertex_.size > 1 ? ReadComponent(p + cs, vertex_.type, false) : 0;
    v->pos[2] = vertex_.size > 2 ? ReadComponent(p + 2 * cs, vertex_.type, false) : 0;
    v->pos[3] = vertex_.size > 3 ? ReadComponent(p + 3 * cs, vertex_.type, false) : 1;
  }
  if (color_.enabled && color_.ptr) {
    const uint8_t* p = at(color_);
    int cs = TypeSize(color_.type);
    float c[4] = {0, 0, 0, 1};
    for (int k = 0; k < color_.size && k < 4; k++)
      c[k] = ReadComponent(p + k * cs, color_.type, true);
    v->color = Argb(c);
  }
  if (normal_.enabled && normal_.ptr) {
    const uint8_t* p = at(normal_);
    int cs = TypeSize(normal_.type);
    for (int k = 0; k < 3; k++) v->normal[k] = ReadComponent(p + k * cs, normal_.type, true);
  }
  for (int u = 0; u < kUnits; u++) {
    const Array& a = texcoord_[u];
    if (!a.enabled || !a.ptr) continue;
    const uint8_t* p = at(a);
    int cs = TypeSize(a.type);
    v->tc[u][0] = ReadComponent(p, a.type, false);
    v->tc[u][1] = a.size > 1 ? ReadComponent(p + cs, a.type, false) : 0;
  }
}

Context::Sources Context::ClientSources() const {
  return {vertex_, normal_, color_, {texcoord_[0], texcoord_[1], texcoord_[2], texcoord_[3]}};
}

void Context::DrawArrays(Enum mode, int first, int count) {
  if (count <= 0) return;
  if (count > 65536) return Unsupported("draw with more than 65536 vertices", mode);
  draw_verts_.resize(size_t(count));
  std::iota(draw_verts_.begin(), draw_verts_.end(), uint32_t(first));
  draw_seq_.resize(size_t(count));
  std::iota(draw_seq_.begin(), draw_seq_.end(), uint16_t(0));
  DrawVertices(mode, ClientSources());
}

void Context::DrawElements(Enum mode, int count, Enum type, const void* indices) {
  if (count <= 0) return;
  const uint8_t* p = static_cast<const uint8_t*>(indices);
  bool ok;
  switch (type) {
    case UNSIGNED_BYTE:
      ok = Compact<uint8_t>(p, size_t(count), remap_, remap_epoch_, draw_verts_, draw_seq_);
      break;
    case UNSIGNED_SHORT:
      ok = Compact<uint16_t>(p, size_t(count), remap_, remap_epoch_, draw_verts_, draw_seq_);
      break;
    case UNSIGNED_INT:
      ok = Compact<uint32_t>(p, size_t(count), remap_, remap_epoch_, draw_verts_, draw_seq_);
      break;
    default: return Unsupported("glDrawElements type", type);
  }
  if (!ok) return Unsupported("draw with more than 65536 vertices", mode);
  DrawVertices(mode, ClientSources());
}

void Context::ArrayElement(int i) {
  Vtx v;
  FetchVertex(i, &v);
  if (in_begin_) immediate_.push_back(v);
}

// ---- immediate mode ---------------------------------------------------------------

void Context::Begin(Enum mode) {
  if (in_begin_) return Error(INVALID_OPERATION, "glBegin");
  in_begin_ = true;
  begin_mode_ = mode;
  immediate_.clear();
}

void Context::End() {
  if (!in_begin_) return Error(INVALID_OPERATION, "glEnd");
  in_begin_ = false;
  size_t n = immediate_.size();
  if (n > 65536) {
    Unsupported("draw with more than 65536 vertices", begin_mode_);
  } else if (n) {
    draw_verts_.resize(n);
    std::iota(draw_verts_.begin(), draw_verts_.end(), 0u);
    draw_seq_.resize(n);
    std::iota(draw_seq_.begin(), draw_seq_.end(), uint16_t(0));
    auto at = [&](const void* field, int size) {
      return Array{true, size, FLOAT, int(sizeof(Vtx)), static_cast<const uint8_t*>(field)};
    };
    const Vtx& v = immediate_[0];
    Sources src{at(v.pos, 4), at(v.normal, 3), at(&v.color, 4), {}};
    for (int u = 0; u < kUnits; u++) src.tc[u] = at(v.tc[u], 2);
    src.d3d_color = true;
    DrawVertices(begin_mode_, src);
  }
  immediate_.clear();
}

void Context::Vertex(float x, float y, float z, float w) {
  current_.pos[0] = x;
  current_.pos[1] = y;
  current_.pos[2] = z;
  current_.pos[3] = w;
  if (in_begin_) immediate_.push_back(current_);
}

void Context::Color(float r, float g, float b, float a) {
  float c[4] = {r, g, b, a};
  current_.color = Argb(c);
}

void Context::TexCoord(int unit, float s, float t) {
  if (unit < 0 || unit >= kUnits) return;
  current_.tc[unit][0] = s;
  current_.tc[unit][1] = t;
}

void Context::Normal(float x, float y, float z) {
  current_.normal[0] = x;
  current_.normal[1] = y;
  current_.normal[2] = z;
}

// ---- drawing -------------------------------------------------------------------------

void Context::DrawVertices(Enum mode, const Sources& src) {
  uint32_t prim = DevicePrim(mode);
  if (!prim) return Unsupported("primitive mode", mode);
  size_t nidx = DeviceIndexCount(mode, draw_seq_.size());
  if (!nidx) return;

  if (!scene_) {
    dev_->BeginScene();
    scene_ = true;
  }
  ApplyState();
  int ntex = ApplyTextures();
  ApplyMatrices();

  // The device layout: position, normal (lighting only), colour, the
  // texture coordinates ApplyTextures' stages read. Each attribute is
  // written for all vertices at once, with its array's type and size
  // resolved first.
  const std::vector<uint32_t>& vs = draw_verts_;
  auto on = [](const Array& a) { return a.enabled && a.ptr; };
  auto stride_of = [](const Array& a) {
    return size_t(a.stride ? a.stride : a.size * TypeSize(a.type));
  };
  // Homogeneous positions (w != 1, e.g. w = 0 for points at infinity)
  // go to the device as they are.
  bool xyzw = !on(src.pos) && current_.pos[3] != 1;
  if (on(src.pos) && src.pos.size > 3)
    ForType(src.pos.type, [&](auto x) {
      using T = decltype(x);
      const uint8_t* w = src.pos.ptr + 3 * TypeSize(src.pos.type);
      size_t st = stride_of(src.pos);
      xyzw = std::any_of(vs.begin(), vs.end(),
                         [&](uint32_t v) { return Load<T>(w + v * st, false) != 1; });
    });
  bool normals = enabled_.count(LIGHTING);
  uint32_t fvf = (xyzw ? FVF_XYZW : FVF_XYZ) | (normals ? FVF_NORMAL : 0) |
                 FVF_DIFFUSE | (uint32_t(ntex) << 8);
  uint32_t pofs = xyzw ? 16 : 12;
  uint32_t nofs = pofs + (normals ? 12 : 0);
  uint32_t stride = nofs + 4 + 8 * uint32_t(ntex);
  size_t nv = vs.size();
  gfx::Device::Mapped m = dev_->MapDraw(uint32_t(nv), stride, uint32_t(nidx));
  uint8_t* d = m.vertices;

  const Array& pos = src.pos;
  if (!on(pos)) {
    Fill(current_.pos, pofs, nv, d, stride);
  } else if (pos.type == FLOAT && pos.size >= 3 && !xyzw) {
    Gather(pos.ptr, stride_of(pos), vs, d, stride,
           [](const uint8_t* p, uint8_t* o) { std::memcpy(o, p, 12); });
  } else if (pos.type == FLOAT && pos.size >= 4) {
    Gather(pos.ptr, stride_of(pos), vs, d, stride,
           [](const uint8_t* p, uint8_t* o) { std::memcpy(o, p, 16); });
  } else {
    ForType(pos.type, [&](auto x) {
      using T = decltype(x);
      int n = std::min(pos.size, 4), cs = TypeSize(pos.type);
      Gather(pos.ptr, stride_of(pos), vs, d, stride, [&](const uint8_t* p, uint8_t* o) {
        float f[4] = {0, 0, 0, 1};
        for (int k = 0; k < n; k++) f[k] = Load<T>(p + k * cs, false);
        std::memcpy(o, f, pofs);
      });
    });
  }

  if (normals) {
    const Array& nrm = src.normal;
    if (!on(nrm)) {
      Fill(current_.normal, 12, nv, d + pofs, stride);
    } else if (nrm.type == FLOAT) {
      Gather(nrm.ptr, stride_of(nrm), vs, d + pofs, stride,
             [](const uint8_t* p, uint8_t* o) { std::memcpy(o, p, 12); });
    } else {
      ForType(nrm.type, [&](auto x) {
        using T = decltype(x);
        int cs = TypeSize(nrm.type);
        Gather(nrm.ptr, stride_of(nrm), vs, d + pofs, stride, [&](const uint8_t* p, uint8_t* o) {
          float f[3];
          for (int k = 0; k < 3; k++) f[k] = Load<T>(p + k * cs, true);
          std::memcpy(o, f, 12);
        });
      });
    }
  }

  // Colours: D3DCOLOR is A8R8G8B8, bytes B, G, R, A in memory. A byte
  // goes through /255 and back to the same byte, so bytes are swizzled.
  const Array& col = src.color;
  if (!on(col)) {
    Fill(&current_.color, 4, nv, d + nofs, stride);
  } else if (src.d3d_color) {
    Gather(col.ptr, stride_of(col), vs, d + nofs, stride,
           [](const uint8_t* p, uint8_t* o) { std::memcpy(o, p, 4); });
  } else if (col.type == UNSIGNED_BYTE && col.size >= 4) {
    Gather(col.ptr, stride_of(col), vs, d + nofs, stride, [](const uint8_t* p, uint8_t* o) {
      uint8_t c[4] = {p[2], p[1], p[0], p[3]};
      std::memcpy(o, c, 4);
    });
  } else if (col.type == UNSIGNED_BYTE && col.size == 3) {
    Gather(col.ptr, stride_of(col), vs, d + nofs, stride, [](const uint8_t* p, uint8_t* o) {
      uint8_t c[4] = {p[2], p[1], p[0], 0xFF};
      std::memcpy(o, c, 4);
    });
  } else {
    ForType(col.type, [&](auto x) {
      using T = decltype(x);
      int n = std::min(col.size, 4), cs = TypeSize(col.type);
      Gather(col.ptr, stride_of(col), vs, d + nofs, stride, [&](const uint8_t* p, uint8_t* o) {
        float c[4] = {0, 0, 0, 1};
        for (int k = 0; k < n; k++) c[k] = Load<T>(p + k * cs, true);
        uint32_t argb = Argb(c);
        std::memcpy(o, &argb, 4);
      });
    });
  }

  for (int u = 0; u < ntex; u++) {
    const Array& tc = src.tc[u];
    uint8_t* dt = d + nofs + 4 + 8 * u;
    if (!on(tc)) {
      Fill(current_.tc[u], 8, nv, dt, stride);
    } else if (tc.type == FLOAT && tc.size >= 2) {
      Gather(tc.ptr, stride_of(tc), vs, dt, stride,
             [](const uint8_t* p, uint8_t* o) { std::memcpy(o, p, 8); });
    } else {
      ForType(tc.type, [&](auto x) {
        using T = decltype(x);
        int size = tc.size, cs = TypeSize(tc.type);
        Gather(tc.ptr, stride_of(tc), vs, dt, stride, [&](const uint8_t* p, uint8_t* o) {
          float f[2] = {Load<T>(p, false), size > 1 ? Load<T>(p + cs, false) : 0};
          std::memcpy(o, f, 8);
        });
      });
    }
  }

  ExpandIndices(mode, draw_seq_, m.indices);
  dev_->DrawMapped(prim, fvf);
}

// ---- readback ------------------------------------------------------------------------

void Context::ReadPixels(int x, int y, int w, int h, Enum format, Enum type, void* out) {
  if (type != UNSIGNED_BYTE) return Unsupported("glReadPixels type", type);
  int comps = format == RGBA || format == BGRA_EXT ? 4 : format == RGB || format == BGR_EXT ? 3 : 0;
  if (!comps) return Unsupported("glReadPixels format", format);
  if (w <= 0 || h <= 0) return;
  std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
  int top = fb_h_ - (y + h);
  dev_->ReadBack({x, top, x + w, top + h}, px.data(), uint32_t(w) * 4,
                 gfx::Format::A8R8G8B8);
  size_t stride = size_t(w) * size_t(comps);
  stride = (stride + size_t(pack_alignment_) - 1) / size_t(pack_alignment_) *
           size_t(pack_alignment_);
  uint8_t* o = static_cast<uint8_t*>(out);
  for (int row = 0; row < h; row++) {
    const uint8_t* s = px.data() + size_t(h - 1 - row) * size_t(w) * 4;  // bottom-up
    uint8_t* d = o + size_t(row) * stride;
    for (int i = 0; i < w; i++, s += 4, d += comps) {
      bool bgr = format == BGRA_EXT || format == BGR_EXT;
      d[0] = bgr ? s[0] : s[2];
      d[1] = s[1];
      d[2] = bgr ? s[2] : s[0];
      if (comps == 4) d[3] = 255;  // no destination alpha
    }
  }
}

}  // namespace recomp::gl
