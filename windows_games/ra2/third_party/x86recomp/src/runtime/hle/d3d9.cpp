// d3d9: Direct3DCreate9, IDirect3D9 and the core of IDirect3DDevice9
// (creation, swap chain, present, state). Resources: d3d9_resource.cpp.
//
// One adapter: the host desktop, presented through the gfx device. State
// calls are cached for the Get* methods; drawing comes with the
// programmable path of the gfx device.
//
// FPU side effect of device creation: unless the app passes
// D3DCREATE_FPU_PRESERVE, the D3D9 runtime switches the calling thread's
// x87 to single precision with all exceptions masked (as D3D7 does; see
// d3d7.cpp). We do the same to the guest FPU and log it.

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "runtime/display.h"
#include "runtime/hle/com.h"
#include "runtime/hle/d3d9_impl.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/present.h"
#include "runtime/hle/user32_window.h"
#include "runtime/log.h"
#include "runtime/x87.h"

namespace recomp::d3d9 {

gfx::Format FormatFromD3D(uint32_t f) {
  using F = gfx::Format;
  switch (f) {
    case D3DFMT_X8R8G8B8: return F::X8R8G8B8;
    case D3DFMT_A8R8G8B8: return F::A8R8G8B8;
    case D3DFMT_R8G8B8: return F::R8G8B8;
    case D3DFMT_R5G6B5: return F::R5G6B5;
    case D3DFMT_X1R5G5B5: return F::X1R5G5B5;
    case D3DFMT_A1R5G5B5: return F::A1R5G5B5;
    case D3DFMT_A4R4G4B4: return F::A4R4G4B4;
    case D3DFMT_X4R4G4B4: return F::X4R4G4B4;
    case D3DFMT_R3G3B2: return F::R3G3B2;
    case D3DFMT_A8: return F::A8;
    case D3DFMT_L8: return F::L8;
    case D3DFMT_A8L8: return F::A8L8;
    case D3DFMT_P8: return F::P8;
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: return F::D16;
    case D3DFMT_D24X8: return F::D24X8;
    case D3DFMT_D24S8: return F::D24S8;
    case D3DFMT_D32: return F::D32;
    case D3DFMT_D15S1: return F::D15S1;
    case D3DFMT_DXT1: return F::DXT1;
    case D3DFMT_DXT2: return F::DXT2;
    case D3DFMT_DXT3: return F::DXT3;
    case D3DFMT_DXT4: return F::DXT4;
    case D3DFMT_DXT5: return F::DXT5;
    case D3DFMT_R16F: return F::R16F;
    case D3DFMT_G16R16F: return F::G16R16F;
    case D3DFMT_A16B16G16R16F: return F::A16B16G16R16F;
    case D3DFMT_R32F: return F::R32F;
    case D3DFMT_G32R32F: return F::G32R32F;
    case D3DFMT_A32B32G32R32F: return F::A32B32G32R32F;
    default: return F::Unknown;
  }
}

bool IsDepthFormat(gfx::Format f) {
  using F = gfx::Format;
  return f == F::D16 || f == F::D24X8 || f == F::D24S8 || f == F::D32 ||
         f == F::D15S1;
}

bool IsBackBufferFormat(uint32_t f) {
  return f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8 || f == D3DFMT_R5G6B5 ||
         f == D3DFMT_X1R5G5B5 || f == D3DFMT_A1R5G5B5;
}

bool IsRenderTargetFormat(uint32_t f) {
  return IsBackBufferFormat(f) || gfx::IsFloat(FormatFromD3D(f));
}

namespace {

using namespace guest;

constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IDirect3D9 = {0x81BDCBCA, 0x64D4, 0x426D, {0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C}};
constexpr GUID kIID_IDirect3DDevice9 = {0xD0223B96, 0xBF7A, 0x43FD, {0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB}};

constexpr uint32_t D3DCREATE_FPU_PRESERVE = 0x2;
constexpr uint32_t D3DPRESENT_INTERVAL_IMMEDIATE = 0x80000000;

// Display (adapter) formats: what a full-screen mode can be.
bool IsDisplayFormat(uint32_t f) {
  return f == D3DFMT_X8R8G8B8 || f == D3DFMT_R5G6B5;
}

D3DCAPS9 Caps() {
  D3DCAPS9 d{};
  d.DeviceType = 1;  // HAL
  d.Caps = 0x20000;  // READ_SCANLINE
  // CANMANAGERESOURCE, DYNAMICTEXTURES, CANAUTOGENMIPMAP, FULLSCREENGAMMA
  d.Caps2 = 0x10000000 | 0x20000000 | 0x40000000 | 0x20000;
  // ALPHA_FULLSCREEN_FLIP_OR_DISCARD, LINEAR_TO_SRGB_PRESENTATION,
  // COPY_TO_VIDMEM, COPY_TO_SYSTEMMEM
  d.Caps3 = 0x20 | 0x80 | 0x100 | 0x200;
  d.PresentationIntervals = 0x80000000 | 0x1 | 0x2 | 0x4 | 0x8;
  d.CursorCaps = 0x1 | 0x2;  // COLOR, LOWRES
  // EXECUTE/TLVERTEX/TEXTURE in system and video memory, DRAWPRIMTLVERTEX,
  // CANRENDERAFTERFLIP, TEXTURENONLOCALVIDMEM, DRAWPRIMITIVES2(EX),
  // HWTRANSFORMANDLIGHT, CANBLTSYSTONONLOCAL, HWRASTERIZATION, PUREDEVICE
  d.DevCaps = 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800 |
              0x1000 | 0x2000 | 0x8000 | 0x10000 | 0x20000 | 0x80000 |
              0x100000;
  // MASKZ, CULLNONE/CW/CCW, COLORWRITEENABLE, CLIPPLANESCALEDPOINTS,
  // CLIPTLVERTS, TSSARGTEMP, BLENDOP, INDEPENDENTWRITEMASKS,
  // PERSTAGECONSTANT, FOGANDSPECULARALPHA, SEPARATEALPHABLEND,
  // MRTINDEPENDENTBITDEPTHS, MRTPOSTPIXELSHADERBLENDING, FOGVERTEXCLAMPED
  d.PrimitiveMiscCaps = 0x2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 |
                        0x400 | 0x800 | 0x4000 | 0x8000 | 0x10000 | 0x20000 |
                        0x40000 | 0x80000 | 0x100000;
  // DITHER, ZTEST, FOGVERTEX, FOGTABLE, MIPMAPLODBIAS, FOGRANGE,
  // ANISOTROPY, WFOG, ZFOG, COLORPERSPECTIVE, SCISSORTEST,
  // SLOPESCALEDEPTHBIAS, DEPTHBIAS, MULTISAMPLE_TOGGLE
  d.RasterCaps = 0x1 | 0x10 | 0x80 | 0x100 | 0x2000 | 0x10000 | 0x20000 |
                 0x100000 | 0x200000 | 0x400000 | 0x1000000 | 0x2000000 |
                 0x4000000 | 0x8000000;
  d.ZCmpCaps = d.AlphaCmpCaps = 0xFF;
  d.SrcBlendCaps = d.DestBlendCaps = 0x3FFF;  // ZERO .. BLENDFACTOR
  // COLORGOURAUDRGB, SPECULARGOURAUDRGB, ALPHAGOURAUDBLEND, FOGGOURAUD
  d.ShadeCaps = 0x8 | 0x200 | 0x4000 | 0x80000;
  // PERSPECTIVE, ALPHA, TEXREPEATNOTSCALEDBYSIZE, PROJECTED, CUBEMAP,
  // VOLUMEMAP, MIPMAP, MIPVOLUMEMAP, MIPCUBEMAP (no POW2: any size)
  d.TextureCaps = 0x1 | 0x4 | 0x8 | 0x400 | 0x800 | 0x2000 | 0x4000 |
                  0x8000 | 0x10000;
  // MIN/MAG point, linear, anisotropic; MIP point, linear
  d.TextureFilterCaps = d.CubeTextureFilterCaps =
      0x100 | 0x200 | 0x400 | 0x10000 | 0x20000 | 0x1000000 | 0x2000000 |
      0x4000000;
  d.VolumeTextureFilterCaps =
      0x100 | 0x200 | 0x10000 | 0x20000 | 0x1000000 | 0x2000000;
  // WRAP, MIRROR, CLAMP, BORDER, INDEPENDENTUV, MIRRORONCE
  d.TextureAddressCaps = d.VolumeTextureAddressCaps = 0x3F;
  d.LineCaps = 0x1 | 0x2 | 0x4 | 0x8 | 0x10;  // TEXTURE ZTEST BLEND ALPHACMP FOG
  d.MaxTextureWidth = d.MaxTextureHeight = 8192;
  d.MaxVolumeExtent = 2048;
  d.MaxTextureRepeat = 8192;
  d.MaxTextureAspectRatio = 8192;
  d.MaxAnisotropy = 16;
  d.MaxVertexW = 1e10f;
  d.GuardBandLeft = d.GuardBandTop = -32768.0f;
  d.GuardBandRight = d.GuardBandBottom = 32768.0f;
  d.StencilCaps = 0x1FF;               // KEEP .. DECR, TWOSIDED
  d.FVFCaps = 8 | 0x80000 | 0x100000;  // 8 sets, DONOTSTRIPELEMENTS, PSIZE
  d.TextureOpCaps = 0x03FFFFFF;
  d.MaxTextureBlendStages = 8;
  d.MaxSimultaneousTextures = 8;
  // TEXGEN, MATERIALSOURCE7, DIRECTIONALLIGHTS, POSITIONALLIGHTS,
  // LOCALVIEWER, TWEENING, TEXGEN_SPHEREMAP
  d.VertexProcessingCaps = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x40 | 0x100;
  d.MaxActiveLights = 8;
  d.MaxUserClipPlanes = 8;
  d.MaxVertexBlendMatrices = 4;
  d.MaxPointSize = 256.0f;
  d.MaxPrimitiveCount = 0xFFFFFF;
  d.MaxVertexIndex = 0xFFFFFF;
  d.MaxStreams = 16;
  d.MaxStreamStride = 508;
  d.VertexShaderVersion = 0xFFFE0300;  // vs_3_0
  d.MaxVertexShaderConst = 256;
  d.PixelShaderVersion = 0xFFFF0300;  // ps_3_0
  d.PixelShader1xMaxValue = 65504.0f;
  // STREAMOFFSET, CAN_STRETCHRECT_FROM_TEXTURES,
  // VERTEXELEMENTSCANSHARESTREAMOFFSET
  d.DevCaps2 = 0x1 | 0x10 | 0x40;
  d.NumberOfAdaptersInGroup = 1;
  d.DeclTypes = 0x3FF;  // UBYTE4 .. FLOAT16_4
  d.NumSimultaneousRTs = 4;
  d.StretchRectFilterCaps = 0x100 | 0x200 | 0x1000000 | 0x2000000;
  d.VS20Caps = {0x1, 24, 32, 4};             // PREDICATION
  d.PS20Caps = {0x1F, 24, 32, 4, 512};       // all 2_x caps
  d.VertexTextureFilterCaps = 0x100 | 0x200 | 0x1000000 | 0x2000000;
  d.MaxVShaderInstructionsExecuted = 65535;
  d.MaxPShaderInstructionsExecuted = 65535;
  d.MaxVertexShader30InstructionSlots = 32768;
  d.MaxPixelShader30InstructionSlots = 32768;
  return d;
}

struct Direct3D : com::Object {};

uint32_t Direct3DVtable() {
  return com::Vtable(
      "d3d9.dll", "IDirect3D9",
      {"QueryInterface", "AddRef", "Release", "RegisterSoftwareDevice",
       "GetAdapterCount", "GetAdapterIdentifier", "GetAdapterModeCount",
       "EnumAdapterModes", "GetAdapterDisplayMode", "CheckDeviceType",
       "CheckDeviceFormat", "CheckDeviceMultiSampleType",
       "CheckDepthStencilMatch", "CheckDeviceFormatConversion",
       "GetDeviceCaps", "GetAdapterMonitor", "CreateDevice"});
}

uint32_t DeviceVtable() {
  return com::Vtable(
      "d3d9.dll", "IDirect3DDevice9",
      {"QueryInterface", "AddRef", "Release", "TestCooperativeLevel",
       "GetAvailableTextureMem", "EvictManagedResources", "GetDirect3D",
       "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
       "SetCursorProperties", "SetCursorPosition", "ShowCursor",
       "CreateAdditionalSwapChain", "GetSwapChain", "GetNumberOfSwapChains",
       "Reset", "Present", "GetBackBuffer", "GetRasterStatus",
       "SetDialogBoxMode", "SetGammaRamp", "GetGammaRamp", "CreateTexture",
       "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer",
       "CreateIndexBuffer", "CreateRenderTarget", "CreateDepthStencilSurface",
       "UpdateSurface", "UpdateTexture", "GetRenderTargetData",
       "GetFrontBufferData", "StretchRect", "ColorFill",
       "CreateOffscreenPlainSurface", "SetRenderTarget", "GetRenderTarget",
       "SetDepthStencilSurface", "GetDepthStencilSurface", "BeginScene",
       "EndScene", "Clear", "SetTransform", "GetTransform",
       "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial",
       "GetMaterial", "SetLight", "GetLight", "LightEnable", "GetLightEnable",
       "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState",
       "CreateStateBlock", "BeginStateBlock", "EndStateBlock",
       "SetClipStatus", "GetClipStatus", "GetTexture", "SetTexture",
       "GetTextureStageState", "SetTextureStageState", "GetSamplerState",
       "SetSamplerState", "ValidateDevice", "SetPaletteEntries",
       "GetPaletteEntries", "SetCurrentTexturePalette",
       "GetCurrentTexturePalette", "SetScissorRect", "GetScissorRect",
       "SetSoftwareVertexProcessing", "GetSoftwareVertexProcessing",
       "SetNPatchMode", "GetNPatchMode", "DrawPrimitive",
       "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP",
       "ProcessVertices", "CreateVertexDeclaration", "SetVertexDeclaration",
       "GetVertexDeclaration", "SetFVF", "GetFVF", "CreateVertexShader",
       "SetVertexShader", "GetVertexShader", "SetVertexShaderConstantF",
       "GetVertexShaderConstantF", "SetVertexShaderConstantI",
       "GetVertexShaderConstantI", "SetVertexShaderConstantB",
       "GetVertexShaderConstantB", "SetStreamSource", "GetStreamSource",
       "SetStreamSourceFreq", "GetStreamSourceFreq", "SetIndices",
       "GetIndices", "CreatePixelShader", "SetPixelShader", "GetPixelShader",
       "SetPixelShaderConstantF", "GetPixelShaderConstantF",
       "SetPixelShaderConstantI", "GetPixelShaderConstantI",
       "SetPixelShaderConstantB", "GetPixelShaderConstantB", "DrawRectPatch",
       "DrawTriPatch", "DeletePatch", "CreateQuery"});
}

Device* Dev(Ctx& c) { return DeviceOf(hle::Arg(c, 0)); }

// Default render, sampler and texture stage states (D3D9 docs).
void InitDefaults(Device& d) {
  auto rs = [&](uint32_t s, uint32_t v) { d.rs[s] = v; };
  rs(7, d.pp.EnableAutoDepthStencil ? 1 : 0);  // ZENABLE
  rs(8, 3);            // FILLMODE solid
  rs(9, 2);            // SHADEMODE gouraud
  rs(14, 1);           // ZWRITEENABLE
  rs(16, 1);           // LASTPIXEL
  rs(19, 2);           // SRCBLEND one
  rs(20, 1);           // DESTBLEND zero
  rs(22, 3);           // CULLMODE ccw
  rs(23, 4);           // ZFUNC lessequal
  rs(25, 8);           // ALPHAFUNC always
  rs(37, 0x3F800000);  // FOGEND 1.0
  rs(38, 0x3F800000);  // FOGDENSITY 1.0
  rs(53, 1);           // STENCILFAIL keep
  rs(54, 1);           // STENCILZFAIL keep
  rs(55, 1);           // STENCILPASS keep
  rs(56, 8);           // STENCILFUNC always
  rs(58, 0xFFFFFFFF);  // STENCILMASK
  rs(59, 0xFFFFFFFF);  // STENCILWRITEMASK
  rs(60, 0xFFFFFFFF);  // TEXTUREFACTOR
  rs(136, 1);          // CLIPPING
  rs(137, 1);          // LIGHTING
  rs(141, 1);          // COLORVERTEX
  rs(142, 1);          // LOCALVIEWER
  rs(145, 1);          // DIFFUSEMATERIALSOURCE color1
  rs(146, 2);          // SPECULARMATERIALSOURCE color2
  rs(154, 0x3F800000);  // POINTSIZE 1.0
  rs(155, 0x3F800000);  // POINTSIZE_MIN 1.0
  rs(158, 0x3F800000);  // POINTSCALE_A 1.0
  rs(161, 1);          // MULTISAMPLEANTIALIAS
  rs(162, 0xFFFFFFFF);  // MULTISAMPLEMASK
  rs(166, 0x42800000);  // POINTSIZE_MAX 64.0
  rs(168, 0xF);        // COLORWRITEENABLE
  rs(171, 1);          // BLENDOP add
  rs(172, 3);          // POSITIONDEGREE cubic
  rs(173, 1);          // NORMALDEGREE linear
  rs(178, 0x3F800000);  // MINTESSELLATIONLEVEL 1.0
  rs(179, 0x3F800000);  // MAXTESSELLATIONLEVEL 1.0
  rs(182, 0x3F800000);  // ADAPTIVETESS_Z 1.0
  rs(186, 1);          // CCW_STENCILFAIL keep
  rs(187, 1);          // CCW_STENCILZFAIL keep
  rs(188, 1);          // CCW_STENCILPASS keep
  rs(189, 8);          // CCW_STENCILFUNC always
  rs(190, 0xF);        // COLORWRITEENABLE1
  rs(191, 0xF);        // COLORWRITEENABLE2
  rs(192, 0xF);        // COLORWRITEENABLE3
  rs(193, 0xFFFFFFFF);  // BLENDFACTOR
  rs(207, 2);          // SRCBLENDALPHA one
  rs(208, 1);          // DESTBLENDALPHA zero
  rs(209, 1);          // BLENDOPALPHA add
  for (auto& s : d.ss) {
    s[1] = s[2] = s[3] = 1;  // ADDRESSU/V/W wrap
    s[5] = s[6] = 1;         // MAGFILTER, MINFILTER point
    s[7] = 0;                // MIPFILTER none
    s[10] = 1;               // MAXANISOTROPY
  }
  for (int s = 0; s < 8; s++) {
    d.tss[s][1] = s == 0 ? 4 : 1;  // COLOROP modulate / disable
    d.tss[s][2] = 2;               // COLORARG1 texture
    d.tss[s][3] = 1;               // COLORARG2 current
    d.tss[s][4] = s == 0 ? 2 : 1;  // ALPHAOP selectarg1 / disable
    d.tss[s][5] = 2;
    d.tss[s][6] = 1;
    d.tss[s][11] = uint32_t(s);    // TEXCOORDINDEX
    d.tss[s][26] = d.tss[s][27] = d.tss[s][28] = 1;  // COLORARG0/ALPHAARG0/RESULTARG current
  }
}

// The swap chain's size and format from the present parameters (zero
// sizes mean the window's client area in windowed mode), written back as
// Windows does.
void ResolvePresentParams(Device& d) {
  D3DPRESENT_PARAMETERS& p = d.pp;
  if (!p.BackBufferWidth || !p.BackBufferHeight) {
    int w = 0, h = 0;
    ClientAreaSize(d.hwnd, &w, &h);
    if (!p.BackBufferWidth) p.BackBufferWidth = UINT(w > 0 ? w : 640);
    if (!p.BackBufferHeight) p.BackBufferHeight = UINT(h > 0 ? h : 480);
  }
  if (p.BackBufferFormat == D3DFMT_UNKNOWN) p.BackBufferFormat = D3DFMT_X8R8G8B8;
  if (!p.BackBufferCount) p.BackBufferCount = 1;
}

bool Vsync(const D3DPRESENT_PARAMETERS& p) {
  if (const char* v = Env("VSYNC")) return *v && *v != '0';
  return p.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE;
}

// (Re)creates the swap chain and the implicit surfaces for d.pp, and binds
// them.
void SetUpSwapChain(Device& d, uint32_t self) {
  ResolvePresentParams(d);
  const D3DPRESENT_PARAMETERS& p = d.pp;
  if (!p.Windowed) {
    SetExclusiveMode(d.hwnd, int(p.BackBufferWidth), int(p.BackBufferHeight));
    // Fullscreen shows the device window: some games create it hidden
    // (ShowWindow(SW_HIDE)) and never show it themselves.
    ShowWindowNoActivate(d.hwnd);
  } else {
    SetExclusiveMode(d.hwnd, 0, 0);
  }
  d.gfx->ResetSwapChain(int(p.BackBufferWidth), int(p.BackBufferHeight), Vsync(p));
  for (uint32_t& rt : d.render_targets) Bind(&rt, 0);
  Bind(&d.depth_stencil, 0);
  if (d.backbuffer) com::Release(d.backbuffer->guest);
  if (d.auto_depth) com::Release(d.auto_depth->guest);
  d.backbuffer.reset();
  d.auto_depth.reset();
  NewSurface(d, self, Surface::Kind::BackBuffer, p.BackBufferWidth,
             p.BackBufferHeight, p.BackBufferFormat, D3DUSAGE_RENDERTARGET,
             D3DPOOL_DEFAULT, &d.backbuffer);
  Bind(&d.render_targets[0], d.backbuffer->guest);
  if (p.EnableAutoDepthStencil) {
    NewSurface(d, self, Surface::Kind::DepthStencil, p.BackBufferWidth,
               p.BackBufferHeight, p.AutoDepthStencilFormat,
               D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, &d.auto_depth);
    Bind(&d.depth_stencil, d.auto_depth->guest);
  }
  d.viewport = {0, 0, p.BackBufferWidth, p.BackBufferHeight, 0.0f, 1.0f};
  d.scissor = {0, 0, LONG(p.BackBufferWidth), LONG(p.BackBufferHeight)};
}

// Releases everything bound to the device (Reset, last Release).
void UnbindAll(Device& d) {
  for (uint32_t& t : d.textures) Bind(&t, 0);
  for (Device::Stream& st : d.streams) {
    Bind(&st.buffer, 0);
    st = {};
  }
  Bind(&d.indices, 0);
  Bind(&d.decl, 0);
  Bind(&d.vs, 0);
  Bind(&d.ps, 0);
}

int g_perf_level = 0;

}  // namespace

uint32_t QueryInterfaceFor(Ctx& c, const guest::GUID& iid) {
  if (com::GuidEq(hle::Arg(c, 1), iid) || com::GuidEq(hle::Arg(c, 1), kIID_IUnknown)) {
    com::AddRef(hle::Arg(c, 0));
    Wr32(hle::Arg(c, 2), hle::Arg(c, 0));
    return D3D_OK;
  }
  Wr32(hle::Arg(c, 2), 0);
  return com::kE_NOINTERFACE;
}

Device* DeviceOf(uint32_t self) {
  Device* d = com::As<Device>(self);
  if (!d) FatalF("IDirect3DDevice9 call on unknown object %08X", self);
  return d;
}

int SamplerIndex(uint32_t s) {
  if (s < 16) return int(s);
  if (s >= 256 && s < 260) return int(s - 240);
  return -1;
}

void Bind(uint32_t* slot, uint32_t obj) {
  if (obj) com::AddRef(obj);
  if (*slot) com::Release(*slot);
  *slot = obj;
}

}  // namespace recomp::d3d9

namespace recomp::hle {
namespace {

using namespace d3d9;
using namespace guest;

HLE("d3d9.dll", Direct3DCreate9, 1) {
  std::fprintf(stderr, "d3d9: Direct3DCreate9(SDK %u)\n", Arg(c, 0));
  EnsureSdlVideo();
  return com::Publish(std::make_shared<Direct3D>(), Direct3DVtable());
}

// The PIX event markers: the nesting level the event starts / ends in.
HLE("d3d9.dll", D3DPERF_BeginEvent, 2) { return uint32_t(g_perf_level++); }
HLE("d3d9.dll", D3DPERF_EndEvent, 0) {
  if (g_perf_level > 0) g_perf_level--;
  return uint32_t(g_perf_level);
}

// ---- IDirect3D9 ------------------------------------------------------------------

HLE_NAMED("d3d9.dll", "IDirect3D9::QueryInterface", D3D9_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IDirect3D9);
}
HLE_NAMED("d3d9.dll", "IDirect3D9::AddRef", D3D9_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("d3d9.dll", "IDirect3D9::Release", D3D9_Release, 1) { return com::Release(Arg(c, 0)); }
HLE_NAMED("d3d9.dll", "IDirect3D9::GetAdapterCount", D3D9_GetAdapterCount, 1) { return 1; }

HLE_NAMED("d3d9.dll", "IDirect3D9::GetAdapterIdentifier", D3D9_GetAdapterIdentifier, 4) {
  if (Arg(c, 1) != 0) return D3DERR_INVALIDCALL;
  D3DADAPTER_IDENTIFIER9* id = G<D3DADAPTER_IDENTIFIER9>(Arg(c, 3));
  std::memset(id, 0, sizeof *id);
  std::snprintf(id->Driver, sizeof id->Driver, "recomp_d3d9.dll");
  std::snprintf(id->Description, sizeof id->Description,
                "x86recomp Direct3D 9 (%s)",
                GfxBackend() == gfx::Backend::Gpu ? "SDL_GPU" : "dxvk-native");
  std::snprintf(id->DeviceName, sizeof id->DeviceName, "\\\\.\\DISPLAY1");
  // 9.18.13.0 (a driver version of the D3D9 era).
  id->DriverVersionHighPart = (9u << 16) | 18u;
  id->DriverVersionLowPart = (13u << 16) | 0u;
  id->DeviceIdentifier = {0x7B2D3B1A, 0x5C8E, 0x4F4D, {0x9A, 0x1E, 0x52, 0x45, 0x43, 0x4F, 0x4D, 0x50}};
  id->WHQLLevel = 1;
  return D3D_OK;
}

void FillMode(D3DDISPLAYMODE* m, int w, int h, uint32_t fmt) {
  m->Width = UINT(w);
  m->Height = UINT(h);
  m->RefreshRate = 60;
  m->Format = fmt;
}

HLE_NAMED("d3d9.dll", "IDirect3D9::GetAdapterModeCount", D3D9_GetAdapterModeCount, 3) {
  if (Arg(c, 1) != 0 || !IsDisplayFormat(Arg(c, 2))) return 0;
  return uint32_t(DisplayModeSizes().size());
}

HLE_NAMED("d3d9.dll", "IDirect3D9::EnumAdapterModes", D3D9_EnumAdapterModes, 5) {
  uint32_t fmt = Arg(c, 2), i = Arg(c, 3);
  std::vector<ModeSize> modes = DisplayModeSizes();
  if (Arg(c, 1) != 0 || !IsDisplayFormat(fmt) || i >= modes.size())
    return D3DERR_INVALIDCALL;
  FillMode(G<D3DDISPLAYMODE>(Arg(c, 4)), modes[i].w, modes[i].h, fmt);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3D9::GetAdapterDisplayMode", D3D9_GetAdapterDisplayMode, 3) {
  if (Arg(c, 1) != 0) return D3DERR_INVALIDCALL;
  int w = 0, h = 0;
  DesktopSize(&w, &h);
  FillMode(G<D3DDISPLAYMODE>(Arg(c, 2)), w, h, D3DFMT_X8R8G8B8);
  return D3D_OK;
}

// (Adapter, DevType, AdapterFormat, BackBufferFormat, Windowed)
HLE_NAMED("d3d9.dll", "IDirect3D9::CheckDeviceType", D3D9_CheckDeviceType, 6) {
  uint32_t adapter_fmt = Arg(c, 3), bb_fmt = Arg(c, 4);
  bool ok = Arg(c, 1) == 0 && Arg(c, 2) == 1 && IsDisplayFormat(adapter_fmt) &&
            (bb_fmt == D3DFMT_UNKNOWN || IsBackBufferFormat(bb_fmt));
  RECOMP_TRACE("  CheckDeviceType(%u, %u, %u) -> %s\n", adapter_fmt, bb_fmt,
               uint32_t(Arg(c, 5)), ok ? "ok" : "not available");
  return ok ? D3D_OK : D3DERR_NOTAVAILABLE;
}

// (Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat)
HLE_NAMED("d3d9.dll", "IDirect3D9::CheckDeviceFormat", D3D9_CheckDeviceFormat, 7) {
  uint32_t usage = Arg(c, 4), rtype = Arg(c, 5), fmt = Arg(c, 6);
  gfx::Format f = FormatFromD3D(fmt);
  bool ok = Arg(c, 1) == 0 && f != gfx::Format::Unknown && f != gfx::Format::P8;
  if (ok && IsDepthFormat(f))
    ok = (usage & D3DUSAGE_DEPTHSTENCIL) && rtype == D3DRTYPE_SURFACE;
  else if (ok && (usage & D3DUSAGE_DEPTHSTENCIL))
    ok = false;
  if (ok && (usage & D3DUSAGE_RENDERTARGET)) ok = IsRenderTargetFormat(fmt);
  if (ok && (usage & (D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE |
                      D3DUSAGE_QUERY_VERTEXTEXTURE)))
    ok = false;
  if (ok && rtype != D3DRTYPE_SURFACE && rtype != D3DRTYPE_TEXTURE &&
      rtype != D3DRTYPE_CUBETEXTURE)
    ok = false;
  RECOMP_TRACE("  CheckDeviceFormat(usage %X, rtype %u, fmt %X) -> %s\n", usage,
               rtype, fmt, ok ? "ok" : "not available");
  return ok ? D3D_OK : D3DERR_NOTAVAILABLE;
}

// (Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType,
// pQualityLevels): no multisampling yet.
HLE_NAMED("d3d9.dll", "IDirect3D9::CheckDeviceMultiSampleType", D3D9_CheckDeviceMultiSampleType, 7) {
  uint32_t type = Arg(c, 5), quality = Arg(c, 6);
  if (type == 0) {
    if (quality) Wr32(quality, 1);
    return D3D_OK;
  }
  if (quality) Wr32(quality, 0);
  return D3DERR_NOTAVAILABLE;
}

// (Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat)
HLE_NAMED("d3d9.dll", "IDirect3D9::CheckDepthStencilMatch", D3D9_CheckDepthStencilMatch, 6) {
  return IsDepthFormat(FormatFromD3D(Arg(c, 5))) && IsRenderTargetFormat(Arg(c, 4))
             ? D3D_OK
             : D3DERR_NOTAVAILABLE;
}

HLE_NAMED("d3d9.dll", "IDirect3D9::GetDeviceCaps", D3D9_GetDeviceCaps, 4) {
  if (Arg(c, 1) != 0 || Arg(c, 2) != 1) return D3DERR_INVALIDCALL;
  *G<D3DCAPS9>(Arg(c, 3)) = Caps();
  return D3D_OK;
}

// (Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
// ppReturnedDeviceInterface)
HLE_NAMED("d3d9.dll", "IDirect3D9::CreateDevice", D3D9_CreateDevice, 7) {
  uint32_t focus = Arg(c, 3), behavior = Arg(c, 4), ppp = Arg(c, 5), out = Arg(c, 6);
  if (Arg(c, 1) != 0 || Arg(c, 2) != 1) return D3DERR_INVALIDCALL;
  auto d = std::make_shared<Device>();
  d->d3d = Arg(c, 0);
  d->creation = {0, 1, focus, behavior};
  d->pp = *G<D3DPRESENT_PARAMETERS>(ppp);
  d->hwnd = d->pp.hDeviceWindow ? d->pp.hDeviceWindow : focus;
  SDL_Window* w = SdlWindowFor(d->hwnd);
  if (!w) FatalF("d3d9: CreateDevice: no SDL window for hwnd %08X", d->hwnd);
  d->gfx = CreateGfxDevice(w);
  uint32_t self = com::Publish(d, DeviceVtable());
  com::AddRef(d->d3d);
  SetUpSwapChain(*d, self);
  *G<D3DPRESENT_PARAMETERS>(ppp) = d->pp;
  InitDefaults(*d);
  BindTargets(*d);
  Wr32(out, self);

  if (!(behavior & D3DCREATE_FPU_PRESERVE)) {
    uint16_t cw = x87::ControlWord(c.fpu);
    uint16_t ncw = uint16_t((cw & ~0x0300u) | 0x003F);  // PC=24, mask all
    x87::SetControlWord(c.fpu, ncw);
    std::fprintf(stderr, "d3d9: CreateDevice set the FPU to single precision "
                         "(cw %04X -> %04X)\n", cw, ncw);
  }
  const D3DPRESENT_PARAMETERS& p = d->pp;
  std::fprintf(stderr,
               "d3d9: CreateDevice %ux%u fmt %u x%u %s, depth %s fmt %u, "
               "interval %X, behavior %X\n",
               p.BackBufferWidth, p.BackBufferHeight, p.BackBufferFormat,
               p.BackBufferCount, p.Windowed ? "windowed" : "fullscreen",
               p.EnableAutoDepthStencil ? "auto" : "none",
               p.AutoDepthStencilFormat, p.PresentationInterval, behavior);
  return D3D_OK;
}

// ---- IDirect3DDevice9 --------------------------------------------------------------

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::QueryInterface", D3DDev9_QueryInterface, 3) {
  return QueryInterfaceFor(c, kIID_IDirect3DDevice9);
}
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::AddRef", D3DDev9_AddRef, 1) { return com::AddRef(Arg(c, 0)); }

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::Release", D3DDev9_Release, 1) {
  uint32_t self = Arg(c, 0);
  Device* d = Dev(c);
  if (d->refs > 1) return com::Release(self);
  // Last reference: what is bound, the implicit surfaces and the
  // IDirect3D9 go with it.
  uint32_t d3d = d->d3d;
  UnbindAll(*d);
  for (uint32_t& rt : d->render_targets) Bind(&rt, 0);
  Bind(&d->depth_stencil, 0);
  if (d->backbuffer) com::Release(d->backbuffer->guest);
  if (d->auto_depth) com::Release(d->auto_depth->guest);
  d->backbuffer.reset();
  d->auto_depth.reset();
  uint32_t left = com::Release(self);
  com::Release(d3d);
  return left;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::TestCooperativeLevel", D3DDev9_TestCooperativeLevel, 1) {
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetAvailableTextureMem", D3DDev9_GetAvailableTextureMem, 1) {
  return Game().video_memory_mb << 20;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::EvictManagedResources", D3DDev9_EvictManagedResources, 1) {
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetDirect3D", D3DDev9_GetDirect3D, 2) {
  uint32_t d3d = Dev(c)->d3d;
  com::AddRef(d3d);
  Wr32(Arg(c, 1), d3d);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetDeviceCaps", D3DDev9_GetDeviceCaps, 2) {
  *G<D3DCAPS9>(Arg(c, 1)) = Caps();
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetDisplayMode", D3DDev9_GetDisplayMode, 3) {
  Device* d = Dev(c);
  if (Arg(c, 1) != 0) return D3DERR_INVALIDCALL;
  int w = int(d->pp.BackBufferWidth), h = int(d->pp.BackBufferHeight);
  if (d->pp.Windowed) DesktopSize(&w, &h);
  FillMode(G<D3DDISPLAYMODE>(Arg(c, 2)), w, h, D3DFMT_X8R8G8B8);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetCreationParameters", D3DDev9_GetCreationParameters, 2) {
  *G<D3DDEVICE_CREATION_PARAMETERS>(Arg(c, 1)) = Dev(c)->creation;
  return D3D_OK;
}

// The D3D cursor is not drawn yet; games that draw their own keep it
// hidden. A visible one with an image is reported once.
void NoteCursor(Device& d) {
  static bool warned = false;
  if (d.cursor_shown && d.cursor_set && !warned) {
    warned = true;
    WarnF("d3d9: the D3D cursor is shown but not drawn yet\n");
  }
}

// (bShow): returns the previous visibility.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::ShowCursor", D3DDev9_ShowCursor, 2) {
  Device* d = Dev(c);
  bool was = d->cursor_shown;
  d->cursor_shown = Arg(c, 1) != 0;
  NoteCursor(*d);
  return was ? 1 : 0;
}

// (XHotSpot, YHotSpot, pCursorBitmap)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetCursorProperties", D3DDev9_SetCursorProperties, 4) {
  Device* d = Dev(c);
  if (!Arg(c, 3)) return D3DERR_INVALIDCALL;
  d->cursor_set = true;
  NoteCursor(*d);
  return D3D_OK;
}

// (X, Y, Flags): the D3D cursor follows the host pointer.
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetCursorPosition", D3DDev9_SetCursorPosition, 4) {
  return 0;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetNumberOfSwapChains", D3DDev9_GetNumberOfSwapChains, 1) {
  return 1;
}

// (pPresentationParameters)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::Reset", D3DDev9_Reset, 2) {
  Device* d = Dev(c);
  d->pp = *G<D3DPRESENT_PARAMETERS>(Arg(c, 1));
  if (!d->pp.hDeviceWindow) d->pp.hDeviceWindow = d->hwnd;
  UnbindAll(*d);
  SetUpSwapChain(*d, Arg(c, 0));
  *G<D3DPRESENT_PARAMETERS>(Arg(c, 1)) = d->pp;
  std::memset(d->rs, 0, sizeof d->rs);
  std::memset(d->ss, 0, sizeof d->ss);
  std::memset(d->tss, 0, sizeof d->tss);
  InitDefaults(*d);
  BindTargets(*d);
  std::fprintf(stderr, "d3d9: Reset %ux%u %s\n", d->pp.BackBufferWidth,
               d->pp.BackBufferHeight, d->pp.Windowed ? "windowed" : "fullscreen");
  return D3D_OK;
}

// (pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::Present", D3DDev9_Present, 5) {
  Device* d = Dev(c);
  PresentFrame(*d->gfx, int(d->pp.BackBufferWidth), int(d->pp.BackBufferHeight));
  return D3D_OK;
}

// (iSwapChain, iBackBuffer, Type, ppBackBuffer)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetBackBuffer", D3DDev9_GetBackBuffer, 5) {
  Device* d = Dev(c);
  if (Arg(c, 1) != 0 || Arg(c, 2) != 0 || !d->backbuffer) {
    Wr32(Arg(c, 4), 0);
    return D3DERR_INVALIDCALL;
  }
  com::AddRef(d->backbuffer->guest);
  Wr32(Arg(c, 4), d->backbuffer->guest);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetRasterStatus", D3DDev9_GetRasterStatus, 3) {
  *G<D3DRASTER_STATUS>(Arg(c, 2)) = {0, 0};
  return D3D_OK;
}

// (index, ppRenderTarget)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetRenderTarget", D3DDev9_GetRenderTarget, 3) {
  Device* d = Dev(c);
  uint32_t i = Arg(c, 1);
  if (i >= 4 || !d->render_targets[i]) {
    Wr32(Arg(c, 2), 0);
    return i < 4 ? D3DERR_NOTFOUND : D3DERR_INVALIDCALL;
  }
  com::AddRef(d->render_targets[i]);
  Wr32(Arg(c, 2), d->render_targets[i]);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetDepthStencilSurface", D3DDev9_GetDepthStencilSurface, 2) {
  Device* d = Dev(c);
  if (!d->depth_stencil) {
    Wr32(Arg(c, 1), 0);
    return D3DERR_NOTFOUND;
  }
  com::AddRef(d->depth_stencil);
  Wr32(Arg(c, 1), d->depth_stencil);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::BeginScene", D3DDev9_BeginScene, 1) {
  Device* d = Dev(c);
  if (d->scene_depth++) return D3DERR_INVALIDCALL;
  d->gfx->BeginScene();
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::EndScene", D3DDev9_EndScene, 1) {
  Device* d = Dev(c);
  if (d->scene_depth == 0) return D3DERR_INVALIDCALL;
  if (--d->scene_depth == 0) d->gfx->EndScene();
  return D3D_OK;
}

// (Count, pRects, Flags, Color, Z, Stencil)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::Clear", D3DDev9_Clear, 7) {
  Device* d = Dev(c);
  uint32_t count = Arg(c, 1), rects = Arg(c, 2);
  float z;
  uint32_t zbits = Arg(c, 5);
  std::memcpy(&z, &zbits, 4);
  std::vector<gfx::Rect> r;
  for (uint32_t i = 0; rects && i < count; i++) {
    const D3DRECT& dr = G<D3DRECT>(rects)[i];
    r.push_back({dr.x1, dr.y1, dr.x2, dr.y2});
  }
  SyncState(*d);
  BindTargets(*d);
  d->gfx->Clear(uint32_t(r.size()), r.empty() ? nullptr : r.data(), Arg(c, 3),
                Arg(c, 4), z, Arg(c, 6));
  return D3D_OK;
}

// (State, Matrix*)
HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetTransform", D3DDev9_SetTransform, 3) {
  std::array<float, 16> m;
  std::memcpy(m.data(), GPtr(Arg(c, 2)), sizeof m);
  Dev(c)->transforms[Arg(c, 1)] = m;
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetTransform", D3DDev9_GetTransform, 3) {
  Device* d = Dev(c);
  std::array<float, 16> m{};
  auto it = d->transforms.find(Arg(c, 1));
  if (it != d->transforms.end())
    m = it->second;
  else
    m[0] = m[5] = m[10] = m[15] = 1.0f;
  std::memcpy(GPtr(Arg(c, 2)), m.data(), sizeof m);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetViewport", D3DDev9_SetViewport, 2) {
  Dev(c)->viewport = *G<D3DVIEWPORT9>(Arg(c, 1));
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetViewport", D3DDev9_GetViewport, 2) {
  *G<D3DVIEWPORT9>(Arg(c, 1)) = Dev(c)->viewport;
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetRenderState", D3DDev9_SetRenderState, 3) {
  uint32_t s = Arg(c, 1);
  if (s >= 256) return D3DERR_INVALIDCALL;
  Dev(c)->rs[s] = Arg(c, 2);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetRenderState", D3DDev9_GetRenderState, 3) {
  uint32_t s = Arg(c, 1);
  if (s >= 256) return D3DERR_INVALIDCALL;
  Wr32(Arg(c, 2), Dev(c)->rs[s]);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetSamplerState", D3DDev9_SetSamplerState, 4) {
  int s = SamplerIndex(Arg(c, 1));
  uint32_t t = Arg(c, 2);
  if (s < 0 || t >= 14) return D3DERR_INVALIDCALL;
  Dev(c)->ss[s][t] = Arg(c, 3);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetSamplerState", D3DDev9_GetSamplerState, 4) {
  int s = SamplerIndex(Arg(c, 1));
  uint32_t t = Arg(c, 2);
  if (s < 0 || t >= 14) return D3DERR_INVALIDCALL;
  Wr32(Arg(c, 3), Dev(c)->ss[s][t]);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetTextureStageState", D3DDev9_SetTextureStageState, 4) {
  uint32_t s = Arg(c, 1), t = Arg(c, 2);
  if (s >= 8 || t >= 33) return D3DERR_INVALIDCALL;
  Dev(c)->tss[s][t] = Arg(c, 3);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetTextureStageState", D3DDev9_GetTextureStageState, 4) {
  uint32_t s = Arg(c, 1), t = Arg(c, 2);
  if (s >= 8 || t >= 33) return D3DERR_INVALIDCALL;
  Wr32(Arg(c, 3), Dev(c)->tss[s][t]);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetScissorRect", D3DDev9_SetScissorRect, 2) {
  Dev(c)->scissor = *G<D3DRECT>(Arg(c, 1));
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetScissorRect", D3DDev9_GetScissorRect, 2) {
  *G<D3DRECT>(Arg(c, 1)) = Dev(c)->scissor;
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetSoftwareVertexProcessing", D3DDev9_SetSoftwareVertexProcessing, 2) {
  Dev(c)->software_vp = BOOL(Arg(c, 1));
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetSoftwareVertexProcessing", D3DDev9_GetSoftwareVertexProcessing, 1) {
  return uint32_t(Dev(c)->software_vp);
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::SetFVF", D3DDev9_SetFVF, 2) {
  Bind(&Dev(c)->decl, 0);
  Dev(c)->fvf = Arg(c, 1);
  return D3D_OK;
}

HLE_NAMED("d3d9.dll", "IDirect3DDevice9::GetFVF", D3DDev9_GetFVF, 2) {
  Wr32(Arg(c, 1), Dev(c)->fvf);
  return D3D_OK;
}

}  // namespace
}  // namespace recomp::hle
