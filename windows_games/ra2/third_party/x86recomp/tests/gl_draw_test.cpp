// The GL context's vertex path (runtime/gl/context.h): what reaches the
// device for glDrawArrays, glDrawElements and immediate mode.
//
// A recording gfx::Device keeps every draw. Each one is expanded to the
// device vertex bytes of every index and compared with a reference model
// written independently here from GL 1.1's rules: component conversion
// per array type (normalized for colours and normals), missing components
// (0, 0, 0, 1), current values for disabled arrays, colours to A8R8G8B8
// through clamp, * 255 and round to nearest, and the device layout
// (position as XYZ, or XYZW when any vertex has w != 1; normal with
// lighting; colour; one texture coordinate set per stage the texture
// units need). XYZW is decided over the vertices the call names, which
// for quads may include a trailing partial one. Vertices and index order may be laid out differently; the
// expanded primitives may not differ.
//
// Also: every UNSIGNED_BYTE colour component comes back as the same byte.
// Scenarios come from raw mt19937 output, which the standard fixes.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "runtime/gl/context.h"
#include "runtime/log.h"

namespace recomp {
bool g_trace = false;
void FatalF(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  std::vfprintf(stderr, fmt, a);
  va_end(a);
  std::abort();
}
void WarnF(const char*, ...) {}
void TraceF(const char*, ...) {}
}  // namespace recomp

namespace {

using namespace recomp;
namespace g = recomp::gl;

struct DrawRec {
  uint32_t prim, fvf, stride;
  std::vector<uint8_t> verts;
  std::vector<uint16_t> idx;
};

class RecordingDevice final : public gfx::Device {
 public:
  std::vector<DrawRec> draws;

  void ResetSwapChain(int, int, bool) override {}
  gfx::TextureId CreateTexture(int, int, int, gfx::Format, bool) override { return ++ids_; }
  void DestroyTexture(gfx::TextureId) override {}
  void FillTexture(gfx::TextureId, const gfx::Rect&, uint32_t) override {}
  void UpdateTexture(gfx::TextureId, int, const gfx::Rect&, const void*, uint32_t) override {}
  gfx::TextureId CreateCubeTexture(int, int, gfx::Format) override { return ++ids_; }
  void UpdateCubeTexture(gfx::TextureId, int, int, const gfx::Rect&, const void*, uint32_t) override {}
  gfx::DepthId CreateDepth(int, int, gfx::Format) override { return 1; }
  void DestroyDepth(gfx::DepthId) override {}
  void SetRenderTarget(gfx::TextureId, gfx::DepthId) override {}
  void SetRenderTargets(const gfx::ColorTarget*, gfx::DepthId) override {}
  void StretchRect(gfx::ColorTarget, const gfx::Rect&, gfx::ColorTarget, const gfx::Rect&, bool) override {}
  gfx::ShaderId CreateShader(const hlsl::Program&) override { return 0; }
  void DestroyShader(gfx::ShaderId) override {}
  void DrawShader(const gfx::ShaderDraw&) override {}
  void Blit(const void*, uint32_t, gfx::Format, const gfx::Rect&, const gfx::Rect&, bool,
            uint32_t) override {}
  void ReadBack(const gfx::Rect&, void*, uint32_t, gfx::Format) override {}
  void BlitBlend(const void*, uint32_t, const gfx::Rect&, const gfx::Rect&) override {}
  uint32_t StartReadBack(const gfx::Rect&) override { return 1; }
  void FinishReadBack(uint32_t, void*, uint32_t, gfx::Format) override {}
  void BlitFront(const void*, uint32_t, gfx::Format, const gfx::Rect&,
                 const gfx::Rect&) override {}
  void ReadBackFront(const gfx::Rect&, void*, uint32_t, gfx::Format) override {}
  void PresentFront() override {}
  void SetRenderState(uint32_t, uint32_t) override {}
  void SetTextureStageState(uint32_t, uint32_t, uint32_t) override {}
  void SetTexture(uint32_t, gfx::TextureId) override {}
  void SetTransform(uint32_t, const float*) override {}
  void SetViewport(const gfx::Viewport&) override {}
  void SetScissor(const gfx::Rect&) override {}
  void SetMaterial(const gfx::Material&) override {}
  void SetLight(uint32_t, const gfx::Light&) override {}
  void LightEnable(uint32_t, bool) override {}
  void SetClipPlane(uint32_t, const float*) override {}
  void Clear(uint32_t, const gfx::Rect*, uint32_t, uint32_t, float, uint32_t) override {}
  void BeginScene() override {}
  void EndScene() override {}
  void Draw(uint32_t prim, uint32_t fvf, const void* v, uint32_t nv, uint32_t stride,
            const uint16_t* idx, uint32_t ni) override {
    auto* b = static_cast<const uint8_t*>(v);
    draws.push_back({prim, fvf, stride, {b, b + size_t(nv) * stride}, {idx, idx + ni}});
  }
  Mapped MapDraw(uint32_t nv, uint32_t stride, uint32_t ni) override {
    // Garbage, so a byte the context does not write shows.
    map_v_.assign(size_t(nv) * stride, 0xCD);
    map_i_.assign(ni, 0xCDCD);
    map_stride_ = stride;
    return {map_v_.data(), map_i_.data()};
  }
  void DrawMapped(uint32_t prim, uint32_t fvf) override {
    draws.push_back({prim, fvf, map_stride_, map_v_, map_i_});
  }
  void Present() override {}

 private:
  uint32_t ids_ = 0;
  std::vector<uint8_t> map_v_;
  std::vector<uint16_t> map_i_;
  uint32_t map_stride_ = 0;
};

// ---- reference model

float Component(const uint8_t* p, g::Enum type, bool normalized) {
  switch (type) {
    case g::UNSIGNED_BYTE: return normalized ? p[0] / 255.0f : float(p[0]);
    case g::BYTE: {
      int v = int8_t(p[0]);
      return normalized ? (2 * v + 1) / 255.0f : float(v);
    }
    case g::UNSIGNED_SHORT: {
      uint16_t v;
      std::memcpy(&v, p, 2);
      return normalized ? v / 65535.0f : float(v);
    }
    case g::SHORT: {
      int16_t v;
      std::memcpy(&v, p, 2);
      return normalized ? (2 * v + 1) / 65535.0f : float(v);
    }
    case g::INT: {
      int32_t v;
      std::memcpy(&v, p, 4);
      return float(v);
    }
    case g::UNSIGNED_INT: {
      uint32_t v;
      std::memcpy(&v, p, 4);
      return float(v);
    }
    case g::FLOAT: {
      float v;
      std::memcpy(&v, p, 4);
      return v;
    }
    case g::DOUBLE: {
      double v;
      std::memcpy(&v, p, 8);
      return float(v);
    }
  }
  return 0;
}

int Bytes(g::Enum t) {
  switch (t) {
    case g::BYTE: case g::UNSIGNED_BYTE: return 1;
    case g::SHORT: case g::UNSIGNED_SHORT: return 2;
    case g::DOUBLE: return 8;
    default: return 4;
  }
}

uint8_t ColorByte(float c) {
  return uint8_t(std::lround(std::min(std::max(c, 0.0f), 1.0f) * 255.0f));
}

struct RefArray {
  bool on = false;
  int size = 4;
  g::Enum type = g::FLOAT;
  int stride = 0;
  const uint8_t* ptr = nullptr;
  const uint8_t* At(uint32_t i) const {
    return ptr + size_t(i) * size_t(stride ? stride : size * Bytes(type));
  }
};

struct RefVertex {
  float pos[4] = {0, 0, 0, 1};
  float normal[3] = {0, 0, 1};
  uint32_t color = 0xFFFFFFFF;
  float tc[g::Context::kUnits][2] = {};
};

struct Model {
  RefArray pos, normal, color, tc[g::Context::kUnits];
  RefVertex current;

  RefVertex Fetch(uint32_t i) const {
    RefVertex v = current;
    if (pos.on) {
      const uint8_t* p = pos.At(i);
      for (int k = 0; k < 4; k++)
        v.pos[k] = k < pos.size ? Component(p + k * Bytes(pos.type), pos.type, false)
                                : k == 3 ? 1.0f : 0.0f;
    }
    if (normal.on)
      for (int k = 0; k < 3; k++)
        v.normal[k] = Component(normal.At(i) + k * Bytes(normal.type), normal.type, true);
    if (color.on) {
      float c[4] = {0, 0, 0, 1};
      for (int k = 0; k < color.size && k < 4; k++)
        c[k] = Component(color.At(i) + k * Bytes(color.type), color.type, true);
      v.color = uint32_t(ColorByte(c[3])) << 24 | uint32_t(ColorByte(c[0])) << 16 |
                uint32_t(ColorByte(c[1])) << 8 | ColorByte(c[2]);
    }
    for (int u = 0; u < g::Context::kUnits; u++)
      if (tc[u].on) {
        const RefArray& a = tc[u];
        v.tc[u][0] = Component(a.At(i), a.type, false);
        v.tc[u][1] = a.size > 1 ? Component(a.At(i) + Bytes(a.type), a.type, false) : 0;
      }
    return v;
  }
};

std::vector<uint32_t> Expand(g::Enum mode, const std::vector<uint32_t>& seq) {
  std::vector<uint32_t> out;
  if (mode == g::QUADS) {
    for (size_t q = 0; q + 3 < seq.size(); q += 4)
      for (size_t k : {0, 1, 2, 0, 2, 3}) out.push_back(seq[q + k]);
  } else {
    out = seq;
    if (mode == g::LINE_LOOP && !seq.empty()) out.push_back(seq[0]);
  }
  return out;
}

uint32_t Prim(g::Enum mode) {
  switch (mode) {
    case g::POINTS: return 1;
    case g::LINES: return 2;
    case g::LINE_STRIP: case g::LINE_LOOP: return 3;
    case g::TRIANGLES: case g::QUADS: return 4;
    case g::TRIANGLE_STRIP: case g::QUAD_STRIP: return 5;
    default: return 6;
  }
}

int g_failures = 0;

void Fail(int scenario, const char* what) {
  if (++g_failures <= 20) std::printf("scenario %d: %s\n", scenario, what);
}

// Checks the last recorded draw against the model's vertices `verts`
// drawn in GL order `seq` (indices into verts).
void Check(int sc, const RecordingDevice& dev, size_t draws_before, g::Enum mode,
           const std::vector<RefVertex>& verts, const std::vector<uint32_t>& seq,
           bool lighting, int ntex) {
  std::vector<uint32_t> want = Expand(mode, seq);
  if (want.empty()) {
    if (dev.draws.size() != draws_before) Fail(sc, "drew an empty primitive");
    return;
  }
  if (dev.draws.size() != draws_before + 1) return Fail(sc, "no draw");
  const DrawRec& d = dev.draws.back();
  bool xyzw = false;
  for (uint32_t i : seq) xyzw |= verts[i].pos[3] != 1;
  uint32_t fvf = (xyzw ? 0x4002u : 0x002u) | (lighting ? 0x010u : 0) | 0x040u |
                 uint32_t(ntex) << 8;
  if (d.prim != Prim(mode)) return Fail(sc, "primitive type");
  if (d.fvf != fvf) return Fail(sc, "FVF");
  uint32_t pbytes = xyzw ? 16 : 12, stride = pbytes + (lighting ? 12 : 0) + 4 + 8 * ntex;
  if (d.stride != stride) return Fail(sc, "stride");
  if (d.idx.size() != want.size()) return Fail(sc, "index count");
  for (size_t k = 0; k < want.size(); k++) {
    if (size_t(d.idx[k]) * stride >= d.verts.size()) return Fail(sc, "index out of range");
    const RefVertex& v = verts[want[k]];
    uint8_t e[64];
    uint8_t* o = e;
    std::memcpy(o, v.pos, pbytes);
    o += pbytes;
    if (lighting) {
      std::memcpy(o, v.normal, 12);
      o += 12;
    }
    std::memcpy(o, &v.color, 4);
    o += 4;
    for (int u = 0; u < ntex; u++, o += 8) std::memcpy(o, v.tc[u], 8);
    if (std::memcmp(e, d.verts.data() + size_t(d.idx[k]) * stride, stride))
      return Fail(sc, "vertex bytes");
  }
}

// ---- scenarios

uint32_t Pick(std::mt19937& r, uint32_t n) { return r() % n; }

float RandFloat(std::mt19937& r) {
  static const float special[] = {0, 1, -1, 0.5f, 2, -0.0f, 255, 1e-3f};
  if (Pick(r, 4) == 0) return special[Pick(r, 8)];
  return (float(r() >> 8) * 0x1p-24f) * 4 - 2;
}

const g::Enum kTypes[] = {g::BYTE, g::UNSIGNED_BYTE, g::SHORT, g::UNSIGNED_SHORT,
                          g::INT, g::UNSIGNED_INT, g::FLOAT, g::DOUBLE};

// Fills `n` elements of `size` components of `type` at `stride` (0:
// packed) in `buf` from offset `ofs`. Integers are raw random bits;
// floats are moderate values, with w = 1 for most positions.
void FillArray(std::mt19937& r, std::vector<uint8_t>& buf, size_t ofs, size_t n, int size,
               g::Enum type, int stride, bool is_pos) {
  size_t st = stride ? size_t(stride) : size_t(size * Bytes(type));
  for (size_t i = 0; i < n; i++)
    for (int k = 0; k < size; k++) {
      uint8_t* p = buf.data() + ofs + i * st + size_t(k * Bytes(type));
      bool w1 = is_pos && k == 3 && Pick(r, 50) != 0;
      if (type == g::FLOAT) {
        float f = w1 ? 1.0f : RandFloat(r);
        std::memcpy(p, &f, 4);
      } else if (type == g::DOUBLE) {
        double f = w1 ? 1.0 : double(RandFloat(r));
        std::memcpy(p, &f, 8);
      } else {
        for (int b = 0; b < Bytes(type); b++) p[b] = uint8_t(r());
        if (w1) {
          uint8_t one[4] = {1, 0, 0, 0};
          std::memcpy(p, one, size_t(Bytes(type)));
        }
      }
    }
}

const g::Enum kModes[] = {g::POINTS, g::LINES, g::LINE_STRIP, g::LINE_LOOP, g::TRIANGLES,
                          g::TRIANGLE_STRIP, g::TRIANGLE_FAN, g::QUADS, g::QUAD_STRIP,
                          g::POLYGON};

struct Arrays {
  std::vector<uint8_t> buf;
  Model m;
};

// Random client arrays over `n` elements: packed or interleaved, every
// type, some disabled.
void SetUpArrays(std::mt19937& r, g::Context& gl, Arrays& a, size_t n) {
  struct Spec {
    RefArray* ref;
    int size;
    g::Enum type;
    bool on;
  };
  a.m = {};
  std::vector<Spec> specs;
  specs.push_back({&a.m.pos, 2 + int(Pick(r, 3)), kTypes[2 + Pick(r, 6)], Pick(r, 10) != 0});
  if (Pick(r, 2)) specs[0].type = g::FLOAT;
  specs.push_back({&a.m.normal, 3, Pick(r, 2) ? g::FLOAT : kTypes[Pick(r, 8)], Pick(r, 2) != 0});
  g::Enum ctype = Pick(r, 2) ? g::UNSIGNED_BYTE : kTypes[Pick(r, 8)];
  specs.push_back({&a.m.color, 3 + int(Pick(r, 2)), ctype, Pick(r, 4) != 0});
  for (int u = 0; u < g::Context::kUnits; u++)
    specs.push_back({&a.m.tc[u], 1 + int(Pick(r, 4)), Pick(r, 2) ? g::FLOAT : kTypes[Pick(r, 8)],
                     Pick(r, 3) != 0});
  bool interleaved = Pick(r, 2);
  int stride = 0;
  if (interleaved)
    for (const Spec& s : specs) stride += s.size * Bytes(s.type);
  stride += interleaved ? int(Pick(r, 3)) * 4 : 0;
  size_t total = 0;
  std::vector<size_t> ofs;
  for (const Spec& s : specs) {
    ofs.push_back(total);
    total += interleaved ? size_t(s.size * Bytes(s.type)) : n * size_t(s.size * Bytes(s.type));
  }
  a.buf.assign(interleaved ? n * size_t(stride) + 8 : total, 0);
  for (size_t i = 0; i < specs.size(); i++) {
    Spec& s = specs[i];
    *s.ref = {s.on, s.size, s.type, interleaved ? stride : 0, a.buf.data() + ofs[i]};
    FillArray(r, a.buf, ofs[i], n, s.size, s.type, interleaved ? stride : 0, i == 0);
  }
  gl.VertexPointer(a.m.pos.size, a.m.pos.type, a.m.pos.stride, a.m.pos.ptr);
  gl.SetClientState(g::VERTEX_ARRAY, a.m.pos.on);
  gl.NormalPointer(a.m.normal.type, a.m.normal.stride, a.m.normal.ptr);
  gl.SetClientState(g::NORMAL_ARRAY, a.m.normal.on);
  gl.ColorPointer(a.m.color.size, a.m.color.type, a.m.color.stride, a.m.color.ptr);
  gl.SetClientState(g::COLOR_ARRAY, a.m.color.on);
  for (int u = 0; u < g::Context::kUnits; u++) {
    gl.ClientActiveTexture(g::TEXTURE0 + g::Enum(u));
    const RefArray& t = a.m.tc[u];
    gl.TexCoordPointer(t.size, t.type, t.stride, t.ptr);
    gl.SetClientState(g::TEXTURE_COORD_ARRAY, t.on);
  }
}

// Enables a random set of texture units (a complete texture each); returns
// how many coordinate sets the device stages need (last enabled unit + 1).
int SetUpTextures(std::mt19937& r, g::Context& gl, const uint32_t* names) {
  int need = 0;
  for (int u = 0; u < g::Context::kUnits; u++) {
    bool on = Pick(r, 3) == 0;
    gl.ActiveTexture(g::TEXTURE0 + g::Enum(u));
    gl.BindTexture(g::TEXTURE_2D, names[u]);
    gl.SetEnabled(g::TEXTURE_2D, on);
    if (on) need = u + 1;
  }
  return need;
}

}  // namespace

int main() {
  // Every byte through /255 and the colour rounding gives itself back.
  for (int b = 0; b < 256; b++) {
    uint8_t p = uint8_t(b);
    if (ColorByte(Component(&p, g::UNSIGNED_BYTE, true)) != b) {
      std::printf("byte %d does not round-trip\n", b);
      return 1;
    }
  }

  RecordingDevice dev;
  g::Context gl(&dev, 640, 480);
  uint32_t names[g::Context::kUnits];
  gl.GenTextures(g::Context::kUnits, names);
  const uint32_t white = 0xFFFFFFFF;
  for (int u = 0; u < g::Context::kUnits; u++) {
    gl.BindTexture(g::TEXTURE_2D, names[u]);
    float linear = float(g::LINEAR);
    gl.TexParameter(g::TEXTURE_2D, g::TEXTURE_MIN_FILTER, &linear);
    gl.TexImage2D(g::TEXTURE_2D, 0, g::RGBA, 1, 1, 0, g::RGBA, g::UNSIGNED_BYTE, &white);
  }

  // All 256 colour bytes in each channel, through glDrawArrays.
  {
    std::vector<uint8_t> rgba(256 * 4);
    std::vector<float> xyz(256 * 3, 0.0f);
    for (int i = 0; i < 256; i++)
      for (int k = 0; k < 4; k++) rgba[size_t(i * 4 + k)] = uint8_t(i + 64 * k);
    gl.VertexPointer(3, g::FLOAT, 0, xyz.data());
    gl.SetClientState(g::VERTEX_ARRAY, true);
    gl.ColorPointer(4, g::UNSIGNED_BYTE, 0, rgba.data());
    gl.SetClientState(g::COLOR_ARRAY, true);
    gl.DrawArrays(g::POINTS, 0, 256);
    const DrawRec& d = dev.draws.back();
    for (int i = 0; i < 256; i++) {
      const uint8_t* c = &rgba[size_t(i * 4)];
      uint32_t want = uint32_t(c[3]) << 24 | uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | c[2];
      uint32_t got;
      std::memcpy(&got, d.verts.data() + size_t(d.idx[size_t(i)]) * d.stride + 12, 4);
      if (got != want) {
        std::printf("colour %d: %08x, want %08x\n", i, got, want);
        return 1;
      }
    }
  }

  std::mt19937 r(12345);
  Arrays a;
  for (int sc = 0; sc < 3000; sc++) {
    bool lighting = Pick(r, 2);
    gl.SetEnabled(g::LIGHTING, lighting);
    int ntex = SetUpTextures(r, gl, names);
    g::Enum mode = kModes[Pick(r, 10)];
    size_t before = dev.draws.size();
    uint32_t kind = Pick(r, 3);
    if (kind == 2) {
      // Immediate mode: current values change between vertices.
      Model m;
      std::vector<RefVertex> verts;
      gl.Begin(mode);
      int n = int(Pick(r, 13));
      for (int i = 0; i < n; i++) {
        if (Pick(r, 2)) {
          float c[4] = {RandFloat(r), RandFloat(r), RandFloat(r), RandFloat(r)};
          gl.Color(c[0], c[1], c[2], c[3]);
          m.current.color = uint32_t(ColorByte(c[3])) << 24 | uint32_t(ColorByte(c[0])) << 16 |
                            uint32_t(ColorByte(c[1])) << 8 | ColorByte(c[2]);
        }
        if (Pick(r, 2)) {
          float s = RandFloat(r), t = RandFloat(r);
          int u = int(Pick(r, g::Context::kUnits));
          gl.TexCoord(u, s, t);
          m.current.tc[u][0] = s;
          m.current.tc[u][1] = t;
        }
        if (Pick(r, 2)) {
          float nx = RandFloat(r), ny = RandFloat(r), nz = RandFloat(r);
          gl.Normal(nx, ny, nz);
          m.current.normal[0] = nx, m.current.normal[1] = ny, m.current.normal[2] = nz;
        }
        float p[4] = {RandFloat(r), RandFloat(r), RandFloat(r), Pick(r, 20) ? 1.0f : 0.0f};
        gl.Vertex(p[0], p[1], p[2], p[3]);
        std::memcpy(m.current.pos, p, sizeof p);
        verts.push_back(m.current);
      }
      gl.End();
      std::vector<uint32_t> seq(verts.size());
      for (size_t i = 0; i < seq.size(); i++) seq[i] = uint32_t(i);
      Check(sc, dev, before, mode, verts, seq, lighting, ntex);
      // Leave the current values where the model's next scenarios expect them.
      gl.Color(1, 1, 1, 1);
      gl.Normal(0, 0, 1);
      for (int u = 0; u < g::Context::kUnits; u++) gl.TexCoord(u, 0, 0);
      gl.Vertex(0, 0, 0, 1);
      continue;
    }
    size_t n = 1 + Pick(r, 300);
    SetUpArrays(r, gl, a, n);
    std::vector<RefVertex> verts(n);
    for (size_t i = 0; i < n; i++) verts[i] = a.m.Fetch(uint32_t(i));
    std::vector<uint32_t> seq;
    if (kind == 0) {
      uint32_t first = Pick(r, uint32_t(n)), count = Pick(r, uint32_t(n - first) + 1);
      for (uint32_t i = 0; i < count; i++) seq.push_back(first + i);
      gl.DrawArrays(mode, int(first), int(count));
    } else {
      // A sparse subset of the arrays, in any order, with repeats.
      size_t count = Pick(r, 400);
      for (size_t i = 0; i < count; i++) seq.push_back(Pick(r, uint32_t(n)));
      g::Enum it = kTypes[1 + 2 * Pick(r, 3)];  // UNSIGNED_BYTE/SHORT/INT
      if (it == g::UNSIGNED_BYTE)
        for (uint32_t& v : seq) v &= 0xFF;
      if (it == g::UNSIGNED_BYTE && n <= 255)
        for (uint32_t& v : seq) v %= uint32_t(n);
      std::vector<uint8_t> ib(seq.size() * size_t(Bytes(it)));
      for (size_t i = 0; i < seq.size(); i++)
        std::memcpy(&ib[i * size_t(Bytes(it))], &seq[i], size_t(Bytes(it)));
      gl.DrawElements(mode, int(seq.size()), it, ib.data());
    }
    if (seq.empty()) continue;
    Check(sc, dev, before, mode, verts, seq, lighting, ntex);
  }
  std::printf("%d failures\n", g_failures);
  return g_failures ? 1 : 0;
}
