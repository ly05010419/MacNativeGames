// d3d7: IDirect3D7, IDirect3DDevice7, IDirect3DVertexBuffer7.
//
// State calls are cached (for the Get* methods) and forwarded to the gfx
// device. Vertex and index data stay in guest memory and are handed to
// the backend as user-pointer draws.
//
// FPU side effect of device creation: unless the app passed
// DDSCL_FPUSETUP or DDSCL_FPUPRESERVE, the DX7 runtime switches the
// calling thread's x87 to single precision (DX7 SDK docs; see
// ARCHITECTURE.md). We do the same to the guest FPU and log it.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>

#include "runtime/hle/ddraw_impl.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"

namespace recomp::ddraw {

namespace {

constexpr guest::GUID kIID_IDirect3DDevice7 = {0xf5049e79, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
constexpr guest::GUID kIID_IDirect3DHALDevice = {0x84E63dE0, 0x46AA, 0x11CF, {0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E}};
constexpr guest::GUID kIID_IDirect3DTnLHalDevice = {0xf5049e78, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};

constexpr uint32_t D3D_OK = 0;
constexpr uint32_t D3DERR_INVALIDCALL = 0x8876086C;

struct Direct3D : com::Object {
  uint32_t ddraw = 0;  // owning IDirectDraw7 guest pointer
};

struct VertexBuffer : com::Object {
  guest::D3DVERTEXBUFFERDESC desc{};
  uint32_t mem = 0;
  uint32_t stride = 0;
  ~VertexBuffer() override {
    if (mem) HeapFreeProcess(mem);
  }
};

struct Device : com::Object {
  uint32_t d3d = 0;       // IDirect3D7 guest pointer
  Surface* target = nullptr;
  uint32_t target_guest = 0;
  bool tnl = true;
  uint32_t rs[256] = {};
  uint32_t tss[8][32] = {};
  uint32_t textures[8] = {};  // guest surface pointers (GetTexture)
  std::map<uint32_t, guest::D3DMATRIX> transforms;
  guest::D3DVIEWPORT7 viewport{};
  guest::D3DMATERIAL7 material{};
  std::map<uint32_t, guest::D3DLIGHT7> lights;
  std::map<uint32_t, bool> light_on;
  float clip_planes[32][4] = {};
};

guest::D3DDEVICEDESC7 DeviceDesc(bool tnl) {
  guest::D3DDEVICEDESC7 d{};
  d.dwDevCaps = 0x1 | 0x10 | 0x40 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 |
                0x8000 | 0x20000 | 0x80000 | (tnl ? 0x10000 : 0);
  guest::D3DPRIMCAPS p{};
  p.dwSize = sizeof p;
  p.dwMiscCaps = 0x2 | 0x10 | 0x20 | 0x40;  // MASKZ, CULLNONE/CW/CCW
  p.dwRasterCaps = 0x1 | 0x10 | 0x80 | 0x100 | 0x2000 | 0x4000 | 0x10000 |
                   0x20000 | 0x100000 | 0x200000;
  p.dwZCmpCaps = 0xFF;
  p.dwSrcBlendCaps = 0x1FFF;
  p.dwDestBlendCaps = 0x1FFF;
  p.dwAlphaCmpCaps = 0xFF;
  p.dwShadeCaps = 0x8 | 0x200 | 0x4000 | 0x80000;
  p.dwTextureCaps = 0x1 | 0x4 | 0x8 | 0x80 | 0x400 | 0x800;
  p.dwTextureFilterCaps = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | 0x200 |
                          0x400 | 0x10000 | 0x20000 | 0x1000000 | 0x2000000 |
                          0x4000000;
  p.dwTextureBlendCaps = 0xFF;
  p.dwTextureAddressCaps = 0x1F;
  d.dpcLineCaps = p;
  d.dpcTriCaps = p;
  d.dwDeviceRenderBitDepth = 0x400 | 0x100;           // 16, 32
  d.dwDeviceZBufferBitDepth = 0x400 | 0x200 | 0x100;  // 16, 24, 32
  d.dwMinTextureWidth = d.dwMinTextureHeight = 1;
  d.dwMaxTextureWidth = d.dwMaxTextureHeight = 2048;
  d.dwMaxTextureRepeat = 8192;
  d.dwMaxTextureAspectRatio = 2048;
  d.dwMaxAnisotropy = 16;
  d.dvGuardBandLeft = d.dvGuardBandTop = -8192.0f;
  d.dvGuardBandRight = d.dvGuardBandBottom = 8192.0f;
  d.dwStencilCaps = 0xFF;
  d.dwFVFCaps = 0x80000 | 8;  // DONOTSTRIPELEMENTS, 8 texcoord sets
  d.dwTextureOpCaps = 0x03FFFFFF;
  d.wMaxTextureBlendStages = 8;
  d.wMaxSimultaneousTextures = 4;
  d.dwMaxActiveLights = 8;
  d.dvMaxVertexW = 1e10f;
  d.deviceGUID = tnl ? kIID_IDirect3DTnLHalDevice : kIID_IDirect3DHALDevice;
  d.wMaxUserClipPlanes = 6;
  d.wMaxVertexBlendMatrices = 4;
  d.dwVertexProcessingCaps = tnl ? (0x1 | 0x2 | 0x8 | 0x10 | 0x20) : 0;
  return d;
}

uint32_t Direct3DVtable() {
  return com::Vtable("ddraw.dll", "IDirect3D7",
                     {"QueryInterface", "AddRef", "Release", "EnumDevices",
                      "CreateDevice", "CreateVertexBuffer",
                      "EnumZBufferFormats", "EvictManagedTextures"});
}

uint32_t DeviceVtable() {
  return com::Vtable(
      "ddraw.dll", "IDirect3DDevice7",
      {"QueryInterface", "AddRef", "Release", "GetCaps", "EnumTextureFormats",
       "BeginScene", "EndScene", "GetDirect3D", "SetRenderTarget",
       "GetRenderTarget", "Clear", "SetTransform", "GetTransform",
       "SetViewport", "MultiplyTransform", "GetViewport", "SetMaterial",
       "GetMaterial", "SetLight", "GetLight", "SetRenderState",
       "GetRenderState", "BeginStateBlock", "EndStateBlock", "PreLoad",
       "DrawPrimitive", "DrawIndexedPrimitive", "SetClipStatus",
       "GetClipStatus", "DrawPrimitiveStrided", "DrawIndexedPrimitiveStrided",
       "DrawPrimitiveVB", "DrawIndexedPrimitiveVB", "ComputeSphereVisibility",
       "GetTexture", "SetTexture", "GetTextureStageState",
       "SetTextureStageState", "ValidateDevice", "ApplyStateBlock",
       "CaptureStateBlock", "DeleteStateBlock", "CreateStateBlock", "Load",
       "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane",
       "GetInfo"});
}

uint32_t VertexBufferVtable() {
  return com::Vtable("ddraw.dll", "IDirect3DVertexBuffer7",
                     {"QueryInterface", "AddRef", "Release", "Lock", "Unlock",
                      "ProcessVertices", "GetVertexBufferDesc", "Optimize",
                      "ProcessVerticesStrided"});
}

Device* Dev(Ctx& c) {
  Device* d = com::As<Device>(hle::Arg(c, 0));
  if (!d) FatalF("IDirect3DDevice7 call on unknown object %08X", uint32_t(hle::Arg(c, 0)));
  return d;
}

void BindTarget(Device& d) {
  Surface* t = d.target;
  gfx::DepthId z = t && t->zbuffer ? t->zbuffer->depth : 0;
  // Back buffer / primary: gfx render target 0.
  Gfx()->SetRenderTarget(t && t->kind == Surface::Kind::Texture ? t->tex : 0, z);
}

// Default D3D7 render states that differ from zero.
void InitDefaults(Device& d) {
  d.rs[7] = 1;       // ZENABLE (true when a Z buffer is attached)
  d.rs[8] = 3;       // FILLMODE solid
  d.rs[9] = 2;       // SHADEMODE gouraud
  d.rs[14] = 1;      // ZWRITEENABLE
  d.rs[16] = 1;      // LASTPIXEL
  d.rs[19] = 2;      // SRCBLEND one
  d.rs[20] = 1;      // DESTBLEND zero
  d.rs[22] = 3;      // CULLMODE ccw
  d.rs[23] = 4;      // ZFUNC lessequal
  d.rs[25] = 8;      // ALPHAFUNC always
  d.rs[136] = 1;     // CLIPPING
  d.rs[137] = 1;     // LIGHTING
  d.rs[141] = 1;     // COLORVERTEX
  d.rs[142] = 1;     // LOCALVIEWER
  d.rs[145] = 1;     // DIFFUSEMATERIALSOURCE color1
  d.rs[146] = 2;     // SPECULARMATERIALSOURCE color2
  for (int s = 0; s < 8; s++) {
    d.tss[s][1] = s == 0 ? 4 : 1;  // COLOROP modulate / disable
    d.tss[s][2] = 2;               // COLORARG1 texture
    d.tss[s][3] = 1;               // COLORARG2 current
    d.tss[s][4] = s == 0 ? 2 : 1;  // ALPHAOP selectarg1 / disable
    d.tss[s][5] = 2;
    d.tss[s][6] = 1;
    d.tss[s][11] = uint32_t(s);    // TEXCOORDINDEX
    d.tss[s][13] = d.tss[s][14] = 1;  // ADDRESSU/V wrap
    d.tss[s][16] = d.tss[s][17] = 1;  // MAG/MINFILTER point
    d.tss[s][18] = 1;                 // MIPFILTER none
  }
}

}  // namespace

uint32_t D3D7For(uint32_t dd) {
  auto d3d = std::make_shared<Direct3D>();
  d3d->ddraw = dd;
  com::AddRef(dd);
  return com::Publish(d3d, Direct3DVtable());
}

}  // namespace recomp::ddraw

namespace recomp::hle {
namespace {

using namespace ddraw;

// ---- IDirect3D7 -------------------------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirect3D7::QueryInterface", D3D7_QueryInterface, 3) {
  if (com::GuidEq(Arg(c, 1), kIID_IDirect3D7)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return D3D_OK;
  }
  Wr32(Arg(c, 2), 0);
  return com::kE_NOINTERFACE;
}

HLE_NAMED("ddraw.dll", "IDirect3D7::AddRef", D3D7_AddRef, 1) { return com::AddRef(Arg(c, 0)); }

HLE_NAMED("ddraw.dll", "IDirect3D7::Release", D3D7_Release, 1) {
  uint32_t self = Arg(c, 0);
  uint32_t dd = 0;
  if (auto* d = com::As<Direct3D>(self)) dd = d->ddraw;
  uint32_t left = com::Release(self);
  if (!left && dd) com::Release(dd);
  return left;
}

// Callback: HRESULT(LPSTR desc, LPSTR name, D3DDEVICEDESC7*, LPVOID ctx).
HLE_NAMED("ddraw.dll", "IDirect3D7::EnumDevices", D3D7_EnumDevices, 3) {
  uint32_t cb = Arg(c, 1), ctx = Arg(c, 2);
  uint32_t buf = HeapAllocProcess(sizeof(guest::D3DDEVICEDESC7) + 128, true);
  uint32_t desc = buf + 128;
  struct {
    const char* desc;
    const char* name;
    bool tnl;
  } devs[] = {{"Microsoft Direct3D Hardware acceleration through Direct3D HAL",
               "Direct3D HAL", false},
              {"Microsoft Direct3D Hardware Transform and Lighting acceleration "
               "capable device",
               "Direct3D T&L HAL", true}};
  for (const auto& d : devs) {
    PutStr(buf, d.desc);
    PutStr(buf + 96, d.name);
    guest::D3DDEVICEDESC7 dd = DeviceDesc(d.tnl);
    std::memcpy(GPtr(desc), &dd, sizeof dd);
    if (!CallGuest(c, cb, {buf, buf + 96, desc, ctx})) break;
  }
  HeapFreeProcess(buf);
  return D3D_OK;
}

// Callback: HRESULT(DDPIXELFORMAT*, LPVOID ctx).
HLE_NAMED("ddraw.dll", "IDirect3D7::EnumZBufferFormats", D3D7_EnumZBufferFormats, 4) {
  uint32_t cb = Arg(c, 2), ctx = Arg(c, 3);
  uint32_t pf = HeapAllocProcess(sizeof(guest::DDPIXELFORMAT), true);
  for (gfx::Format f : {gfx::Format::D16, gfx::Format::D24X8, gfx::Format::D24S8}) {
    PixelFormatFromFormat(f, G<guest::DDPIXELFORMAT>(pf));
    if (!CallGuest(c, cb, {pf, ctx})) break;
  }
  HeapFreeProcess(pf);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3D7::EvictManagedTextures", D3D7_EvictManagedTextures, 1) {
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3D7::CreateDevice", D3D7_CreateDevice, 4) {
  uint32_t iid = Arg(c, 1), surf = Arg(c, 2), out = Arg(c, 3);
  Surface* t = com::As<Surface>(surf);
  if (!t) return D3DERR_INVALIDCALL;
  auto d = std::make_shared<Device>();
  d->d3d = Arg(c, 0);
  d->tnl = com::GuidEq(iid, kIID_IDirect3DTnLHalDevice);
  d->target = t;
  d->target_guest = surf;
  com::AddRef(surf);
  com::AddRef(d->d3d);
  InitDefaults(*d);
  d->rs[7] = t->zbuffer ? 1 : 0;
  d->viewport = {0, 0, uint32_t(t->width), uint32_t(t->height), 0.0f, 1.0f};
  BindTarget(*d);
  Gfx()->SetViewport({0, 0, uint32_t(t->width), uint32_t(t->height), 0.0f, 1.0f});
  Wr32(out, com::Publish(d, DeviceVtable()));

  uint32_t coop = St().coop_flags;
  if (!(coop & (0x800 | 0x1000))) {  // neither FPUSETUP nor FPUPRESERVE
    uint16_t cw = x87::ControlWord(c.fpu);
    uint16_t ncw = uint16_t((cw & ~0x0300u) | 0x003F);  // PC=24, mask all
    x87::SetControlWord(c.fpu, ncw);
    std::fprintf(stderr, "d3d7: CreateDevice set the FPU to single precision "
                         "(cw %04X -> %04X)\n", cw, ncw);
  }
  std::fprintf(stderr, "d3d7: CreateDevice(%s) on %dx%d target, coop flags %X\n",
               d->tnl ? "T&L HAL" : "HAL", t->width, t->height, coop);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3D7::CreateVertexBuffer", D3D7_CreateVertexBuffer, 4) {
  const guest::D3DVERTEXBUFFERDESC& in = *G<guest::D3DVERTEXBUFFERDESC>(Arg(c, 1));
  auto vb = std::make_shared<VertexBuffer>();
  vb->desc = in;
  vb->stride = gfx::FvfStride(in.dwFVF);
  vb->mem = HeapAllocProcess(std::max(1u, vb->stride * in.dwNumVertices), true);
  Wr32(Arg(c, 2), com::Publish(vb, VertexBufferVtable()));
  return D3D_OK;
}

// ---- IDirect3DVertexBuffer7 ---------------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirect3DVertexBuffer7::AddRef", VB7_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirect3DVertexBuffer7::Release", VB7_Release, 1) { return com::Release(Arg(c, 0)); }

HLE_NAMED("ddraw.dll", "IDirect3DVertexBuffer7::Lock", VB7_Lock, 4) {
  auto* vb = com::As<VertexBuffer>(Arg(c, 0));
  if (!vb) return D3DERR_INVALIDCALL;
  Wr32(Arg(c, 2), vb->mem);
  if (Arg(c, 3)) Wr32(Arg(c, 3), vb->stride * vb->desc.dwNumVertices);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DVertexBuffer7::Unlock", VB7_Unlock, 1) { return D3D_OK; }
HLE_NAMED("ddraw.dll", "IDirect3DVertexBuffer7::Optimize", VB7_Optimize, 3) { return D3D_OK; }

HLE_NAMED("ddraw.dll", "IDirect3DVertexBuffer7::GetVertexBufferDesc", VB7_GetVertexBufferDesc, 2) {
  auto* vb = com::As<VertexBuffer>(Arg(c, 0));
  if (!vb) return D3DERR_INVALIDCALL;
  std::memcpy(GPtr(Arg(c, 1)), &vb->desc, sizeof vb->desc);
  return D3D_OK;
}

// ---- IDirect3DDevice7 ----------------------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::QueryInterface", D3DD7_QueryInterface, 3) {
  if (com::GuidEq(Arg(c, 1), kIID_IDirect3DDevice7)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return D3D_OK;
  }
  Wr32(Arg(c, 2), 0);
  return com::kE_NOINTERFACE;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::AddRef", D3DD7_AddRef, 1) { return com::AddRef(Arg(c, 0)); }

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::Release", D3DD7_Release, 1) {
  uint32_t self = Arg(c, 0);
  uint32_t surf = 0, d3d = 0;
  if (auto* d = com::As<Device>(self)) {
    surf = d->target_guest;
    d3d = d->d3d;
  }
  uint32_t left = com::Release(self);
  if (!left) {
    if (surf) com::Release(surf);
    if (d3d) com::Release(d3d);
  }
  return left;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetCaps", D3DD7_GetCaps, 2) {
  guest::D3DDEVICEDESC7 d = DeviceDesc(Dev(c)->tnl);
  std::memcpy(GPtr(Arg(c, 1)), &d, sizeof d);
  return D3D_OK;
}

// Callback: HRESULT(DDPIXELFORMAT*, LPVOID ctx).
HLE_NAMED("ddraw.dll", "IDirect3DDevice7::EnumTextureFormats", D3DD7_EnumTextureFormats, 3) {
  uint32_t cb = Arg(c, 1), ctx = Arg(c, 2);
  uint32_t pf = HeapAllocProcess(sizeof(guest::DDPIXELFORMAT), true);
  using F = gfx::Format;
  for (F f : {F::A8R8G8B8, F::X8R8G8B8, F::R5G6B5, F::X1R5G5B5, F::A1R5G5B5,
              F::A4R4G4B4, F::DXT1, F::DXT2, F::DXT3, F::DXT4, F::DXT5}) {
    PixelFormatFromFormat(f, G<guest::DDPIXELFORMAT>(pf));
    if (!CallGuest(c, cb, {pf, ctx})) break;
  }
  HeapFreeProcess(pf);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::BeginScene", D3DD7_BeginScene, 1) {
  Dev(c);
  Gfx()->BeginScene();
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::EndScene", D3DD7_EndScene, 1) {
  Dev(c);
  Gfx()->EndScene();
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetDirect3D", D3DD7_GetDirect3D, 2) {
  Device* d = Dev(c);
  com::AddRef(d->d3d);
  Wr32(Arg(c, 1), d->d3d);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::SetRenderTarget", D3DD7_SetRenderTarget, 3) {
  Device* d = Dev(c);
  Surface* t = com::As<Surface>(Arg(c, 1));
  if (!t) return D3DERR_INVALIDCALL;
  com::AddRef(Arg(c, 1));
  uint32_t old = d->target_guest;
  d->target = t;
  d->target_guest = Arg(c, 1);
  if (old) com::Release(old);
  BindTarget(*d);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetRenderTarget", D3DD7_GetRenderTarget, 2) {
  Device* d = Dev(c);
  com::AddRef(d->target_guest);
  Wr32(Arg(c, 1), d->target_guest);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::Clear", D3DD7_Clear, 7) {
  Dev(c);
  uint32_t count = Arg(c, 1), rects = Arg(c, 2), flags = Arg(c, 3),
           color = Arg(c, 4), zbits = Arg(c, 5), stencil = Arg(c, 6);
  float z;
  std::memcpy(&z, &zbits, 4);
  Gfx()->Clear(count, rects ? G<gfx::Rect>(rects) : nullptr, flags, color, z,
               stencil);
  return D3D_OK;
}

// The current matrix of a transform state; identity until set.
guest::D3DMATRIX Transform(const Device& d, uint32_t type) {
  guest::D3DMATRIX m{};
  auto it = d.transforms.find(type);
  if (it != d.transforms.end()) return it->second;
  for (int i = 0; i < 4; i++) m.m[i][i] = 1.0f;
  return m;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::SetTransform", D3DD7_SetTransform, 3) {
  Device* d = Dev(c);
  const guest::D3DMATRIX& m = *G<guest::D3DMATRIX>(Arg(c, 2));
  d->transforms[Arg(c, 1)] = m;
  Gfx()->SetTransform(Arg(c, 1), &m.m[0][0]);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetTransform", D3DD7_GetTransform, 3) {
  guest::D3DMATRIX m = Transform(*Dev(c), Arg(c, 1));
  std::memcpy(GPtr(Arg(c, 2)), &m, sizeof m);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::MultiplyTransform", D3DD7_MultiplyTransform, 3) {
  Device* d = Dev(c);
  guest::D3DMATRIX a = Transform(*d, Arg(c, 1));
  const guest::D3DMATRIX& b = *G<guest::D3DMATRIX>(Arg(c, 2));
  // The new state is the given matrix times the current one (row-major
  // D3DMATRIX): "the multiplication order is pMatrix times State". The
  // reverse order misplaced every object the game positions this way
  // (animated models snapping, the waterwheel flying off).
  guest::D3DMATRIX r{};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      for (int k = 0; k < 4; k++) r.m[i][j] += b.m[i][k] * a.m[k][j];
  d->transforms[Arg(c, 1)] = r;
  Gfx()->SetTransform(Arg(c, 1), &r.m[0][0]);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::SetViewport", D3DD7_SetViewport, 2) {
  Device* d = Dev(c);
  d->viewport = *G<guest::D3DVIEWPORT7>(Arg(c, 1));
  const auto& v = d->viewport;
  Gfx()->SetViewport({v.dwX, v.dwY, v.dwWidth, v.dwHeight, v.dvMinZ, v.dvMaxZ});
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetViewport", D3DD7_GetViewport, 2) {
  std::memcpy(GPtr(Arg(c, 1)), &Dev(c)->viewport, sizeof(guest::D3DVIEWPORT7));
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::SetMaterial", D3DD7_SetMaterial, 2) {
  Device* d = Dev(c);
  d->material = *G<guest::D3DMATERIAL7>(Arg(c, 1));
  gfx::Material m;
  static_assert(sizeof m == sizeof d->material);
  std::memcpy(&m, &d->material, sizeof m);
  Gfx()->SetMaterial(m);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetMaterial", D3DD7_GetMaterial, 2) {
  std::memcpy(GPtr(Arg(c, 1)), &Dev(c)->material, sizeof(guest::D3DMATERIAL7));
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::SetLight", D3DD7_SetLight, 3) {
  Device* d = Dev(c);
  const guest::D3DLIGHT7& l = *G<guest::D3DLIGHT7>(Arg(c, 2));
  d->lights[Arg(c, 1)] = l;
  gfx::Light gl;
  static_assert(sizeof gl == sizeof l);
  std::memcpy(&gl, &l, sizeof gl);
  Gfx()->SetLight(Arg(c, 1), gl);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetLight", D3DD7_GetLight, 3) {
  Device* d = Dev(c);
  auto it = d->lights.find(Arg(c, 1));
  if (it == d->lights.end()) return D3DERR_INVALIDCALL;
  std::memcpy(GPtr(Arg(c, 2)), &it->second, sizeof it->second);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::LightEnable", D3DD7_LightEnable, 3) {
  Device* d = Dev(c);
  d->light_on[Arg(c, 1)] = Arg(c, 2) != 0;
  Gfx()->LightEnable(Arg(c, 1), Arg(c, 2) != 0);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetLightEnable", D3DD7_GetLightEnable, 3) {
  Device* d = Dev(c);
  Wr32(Arg(c, 2), d->light_on[Arg(c, 1)] ? 1 : 0);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::SetRenderState", D3DD7_SetRenderState, 3) {
  Device* d = Dev(c);
  uint32_t s = Arg(c, 1), v = Arg(c, 2);
  if (s < 256) d->rs[s] = v;
  Gfx()->SetRenderState(s, v);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetRenderState", D3DD7_GetRenderState, 3) {
  Device* d = Dev(c);
  uint32_t s = Arg(c, 1);
  Wr32(Arg(c, 2), s < 256 ? d->rs[s] : 0);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::SetTextureStageState", D3DD7_SetTextureStageState, 4) {
  Device* d = Dev(c);
  uint32_t st = Arg(c, 1), type = Arg(c, 2), v = Arg(c, 3);
  if (st < 8 && type < 32) d->tss[st][type] = v;
  Gfx()->SetTextureStageState(st, type, v);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetTextureStageState", D3DD7_GetTextureStageState, 4) {
  Device* d = Dev(c);
  uint32_t st = Arg(c, 1), type = Arg(c, 2);
  Wr32(Arg(c, 3), st < 8 && type < 32 ? d->tss[st][type] : 0);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::SetTexture", D3DD7_SetTexture, 3) {
  Device* d = Dev(c);
  uint32_t st = Arg(c, 1), surf = Arg(c, 2);
  if (st >= 8) return D3DERR_INVALIDCALL;
  Surface* s = surf ? com::As<Surface>(surf) : nullptr;
  if (surf && !s) return D3DERR_INVALIDCALL;
  if (surf) com::AddRef(surf);
  if (d->textures[st]) com::Release(d->textures[st]);
  d->textures[st] = surf;
  Gfx()->SetTexture(st, s ? s->tex : 0);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetTexture", D3DD7_GetTexture, 3) {
  Device* d = Dev(c);
  uint32_t st = Arg(c, 1);
  uint32_t t = st < 8 ? d->textures[st] : 0;
  if (t) com::AddRef(t);
  Wr32(Arg(c, 2), t);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::ValidateDevice", D3DD7_ValidateDevice, 2) {
  Wr32(Arg(c, 1), 1);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::SetClipPlane", D3DD7_SetClipPlane, 3) {
  Device* d = Dev(c);
  uint32_t i = Arg(c, 1);
  if (i >= 32) return D3DERR_INVALIDCALL;
  std::memcpy(d->clip_planes[i], GPtr(Arg(c, 2)), 16);
  Gfx()->SetClipPlane(i, d->clip_planes[i]);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::GetClipPlane", D3DD7_GetClipPlane, 3) {
  Device* d = Dev(c);
  uint32_t i = Arg(c, 1);
  if (i >= 32) return D3DERR_INVALIDCALL;
  std::memcpy(GPtr(Arg(c, 2)), d->clip_planes[i], 16);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::PreLoad", D3DD7_PreLoad, 2) { return D3D_OK; }

// ---- drawing ---------------------------------------------------------------------------

// <ID>_CHECK_INDICES=1: report indexed draws whose indices reach past the
// vertex count they pass (D3D7 does not enforce it; d3d9 UP draws do).
void CheckIndices(const char* fn, const uint16_t* idx, uint32_t ni, uint32_t nv) {
  static const bool on = Env("CHECK_INDICES") != nullptr;
  if (!on) return;
  uint32_t mx = 0;
  for (uint32_t i = 0; i < ni; i++) mx = std::max<uint32_t>(mx, idx[i]);
  static int reported = 0;
  if (mx >= nv && reported++ < 20)
    WarnF("%s: max index %u >= vertex count %u (%u indices)\n", fn, mx, nv, ni);
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::DrawPrimitive", D3DD7_DrawPrimitive, 6) {
  Dev(c);
  uint32_t prim = Arg(c, 1), fvf = Arg(c, 2), verts = Arg(c, 3), n = Arg(c, 4);
  Gfx()->Draw(prim, fvf, GPtr(verts), n, gfx::FvfStride(fvf), nullptr, 0);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::DrawIndexedPrimitive", D3DD7_DrawIndexedPrimitive, 8) {
  Dev(c);
  uint32_t prim = Arg(c, 1), fvf = Arg(c, 2), verts = Arg(c, 3), nv = Arg(c, 4),
           idx = Arg(c, 5), ni = Arg(c, 6);
  CheckIndices("DrawIndexedPrimitive", reinterpret_cast<const uint16_t*>(GPtr(idx)), ni, nv);
  Gfx()->Draw(prim, fvf, GPtr(verts), nv, gfx::FvfStride(fvf),
              reinterpret_cast<const uint16_t*>(GPtr(idx)), ni);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::DrawPrimitiveVB", D3DD7_DrawPrimitiveVB, 6) {
  Dev(c);
  auto* vb = com::As<VertexBuffer>(Arg(c, 2));
  if (!vb) return D3DERR_INVALIDCALL;
  uint32_t prim = Arg(c, 1), start = Arg(c, 3), n = Arg(c, 4);
  Gfx()->Draw(prim, vb->desc.dwFVF, GPtr(vb->mem + start * vb->stride), n,
              vb->stride, nullptr, 0);
  return D3D_OK;
}

HLE_NAMED("ddraw.dll", "IDirect3DDevice7::DrawIndexedPrimitiveVB", D3DD7_DrawIndexedPrimitiveVB, 8) {
  Dev(c);
  auto* vb = com::As<VertexBuffer>(Arg(c, 2));
  if (!vb) return D3DERR_INVALIDCALL;
  uint32_t prim = Arg(c, 1), start = Arg(c, 3), nv = Arg(c, 4), idx = Arg(c, 5),
           ni = Arg(c, 6);
  CheckIndices("DrawIndexedPrimitiveVB", reinterpret_cast<const uint16_t*>(GPtr(idx)), ni, nv);
  Gfx()->Draw(prim, vb->desc.dwFVF, GPtr(vb->mem + start * vb->stride), nv,
              vb->stride, reinterpret_cast<const uint16_t*>(GPtr(idx)), ni);
  return D3D_OK;
}

}  // namespace
}  // namespace recomp::hle
