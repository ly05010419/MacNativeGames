// ddraw: DirectDrawEnumerateExA/DirectDrawCreateEx, IDirectDraw7,
// IDirectDrawSurface7, IDirectDrawGammaControl, IDirectDrawClipper.
//
// Surfaces:
//   - Back buffer: the gfx render target. The game presents with
//     Blt(primary <- back buffer) (or Flip). Its guest memory mirrors it
//     lazily (lazy_mem.h): Lock reads back only the rows the game
//     touches, Unlock writes back the rows it changed.
//   - Primary: the gfx front buffer (the last presented frame), which the
//     game's cursor thread draws on between frames.
//   - Z buffers: gfx depth surfaces, attached to the back buffer.
//   - Textures (incl. mip chains): a gfx texture plus guest memory for each
//     level; Lock hands out the guest memory, Unlock uploads it.
//   - Offscreen plain / system memory: guest memory only; Blt to the back
//     buffer goes through gfx::Device::Blit.
// The adapter is reported as a GeForce3 with 64 MB, the best-supported
// profile in the game's system_detail.gas (trilinear filtering,
// render-target shadows).

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

#include "runtime/hle/ddraw_impl.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/input_state.h"
#include "runtime/hle/present.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_window.h"
#include "runtime/log.h"
#include "runtime/watchdog.h"

namespace recomp::ddraw {

using guest::DDPIXELFORMAT;
using guest::DDSURFACEDESC2;

namespace {

constexpr guest::GUID kIID_IDirectDraw7 = {0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
constexpr guest::GUID kIID_IDirectDrawSurface7 = {0x06675a80, 0x3b9b, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
constexpr guest::GUID kIID_IDirectDrawGammaControl = {0x69C11C3E, 0xB46B, 0x11D1, {0xAD, 0x7A, 0x00, 0xC0, 0x4F, 0xC2, 0x9B, 0x4E}};

// GameConfig::video_memory_mb, with 8 MB in use (the primary surface).
uint32_t VidMemTotal() { return Game().video_memory_mb << 20; }
uint32_t VidMemFree() { return VidMemTotal() - (8u << 20); }

State g_state;
Uint64 g_last_present_ms = 0;  // SDL ticks of the last frame Present
// Pushes to the primary not shown yet, and when the first of them came. A
// frame is many pushes (the picture, then the cursor over it): they are
// shown together at the next event pump (SetFrameEndHandler) or frame end
// the game reports (GameFrameEnd; then shown even when unchanged), or
// after kFrontStaleMs when the game draws without pumping (loading screens).
std::atomic<bool> g_front_pending{false};
std::atomic<Uint64> g_front_pending_ms{0};
constexpr Uint64 kFrontStaleMs = 100;

void PresentPendingFront(bool again) {
  if (!g_front_pending.exchange(false) && !again) return;
  Gfx()->PresentFront();
  hle::NoteFrameTiming();
}

int PrimaryWidth() { return g_state.mode_w ? g_state.mode_w : 1024; }
int PrimaryHeight() { return g_state.mode_h ? g_state.mode_h : 768; }
gfx::Format PrimaryFormat() {
  return g_state.mode_bpp == 16 ? gfx::Format::R5G6B5 : gfx::Format::X8R8G8B8;
}

void PresentPrimary() {
  g_state.front_from_gpu = true;
  hle::PresentFrame(*Gfx(), PrimaryWidth(), PrimaryHeight());
  g_last_present_ms = SDL_GetTicks();
}

}  // namespace

State& St() { return g_state; }

gfx::Device* Gfx() {
  State& s = g_state;
  if (!s.gfx) {
    // No vsync unless <ID>_VSYNC=1: a FIFO present that the compositor
    // stops releasing (no frame callbacks) can hang a back-buffer readback
    // indefinitely.
    const char* vs = Env("VSYNC");
    s.vsync = vs && *vs && *vs != '0';
    SDL_Window* w = SdlWindowFor(s.hwnd);
    if (!w) FatalF("ddraw: no SDL window for hwnd %08X", s.hwnd);
    s.gfx = hle::CreateGfxDevice(w);
    s.gfx->ResetSwapChain(PrimaryWidth(), PrimaryHeight(), s.vsync);
    SetFrameEndHandler(PresentPendingFront);
  }
  return s.gfx.get();
}

// ---- pixel formats ------------------------------------------------------------------

gfx::Format FormatFromPixelFormat(const DDPIXELFORMAT& pf) {
  using F = gfx::Format;
  if (pf.dwFlags & DDPF_FOURCC) {
    switch (pf.dwFourCC) {
      case 0x31545844: return F::DXT1;
      case 0x32545844: return F::DXT2;
      case 0x33545844: return F::DXT3;
      case 0x34545844: return F::DXT4;
      case 0x35545844: return F::DXT5;
      default: return F::Unknown;
    }
  }
  if (pf.dwFlags & DDPF_ZBUFFER) {
    uint32_t bits = pf.dwRGBBitCount, stencil = pf.dwRBitMask;
    if (bits == 16) return stencil ? F::D15S1 : F::D16;
    if (bits == 32 && pf.dwGBitMask != 0xFFFFFFFF) return stencil ? F::D24S8 : F::D24X8;
    if (bits == 32) return F::D32;
    if (bits == 24) return F::D24X8;
    return F::Unknown;
  }
  if (pf.dwFlags & DDPF_PALETTEINDEXED8) return F::P8;
  if (pf.dwFlags & DDPF_LUMINANCE)
    return (pf.dwFlags & DDPF_ALPHAPIXELS) ? F::A8L8 : F::L8;
  if ((pf.dwFlags & DDPF_ALPHA) && !(pf.dwFlags & DDPF_RGB)) return F::A8;
  if (pf.dwFlags & DDPF_RGB) {
    bool alpha = (pf.dwFlags & DDPF_ALPHAPIXELS) && pf.dwRGBAlphaBitMask;
    switch (pf.dwRGBBitCount) {
      case 32: return alpha ? F::A8R8G8B8 : F::X8R8G8B8;
      case 24: return F::R8G8B8;
      case 16:
        if (pf.dwRBitMask == 0xF800) return F::R5G6B5;
        if (pf.dwRBitMask == 0x7C00) return alpha ? F::A1R5G5B5 : F::X1R5G5B5;
        if (pf.dwRBitMask == 0x0F00) return alpha ? F::A4R4G4B4 : F::X4R4G4B4;
        return F::Unknown;
      case 8: return F::R3G3B2;
    }
  }
  return F::Unknown;
}

void PixelFormatFromFormat(gfx::Format f, DDPIXELFORMAT* pf) {
  using F = gfx::Format;
  std::memset(pf, 0, sizeof *pf);
  pf->dwSize = sizeof *pf;
  auto rgb = [&](uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    pf->dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
    pf->dwRGBBitCount = bits;
    pf->dwRBitMask = r;
    pf->dwGBitMask = g;
    pf->dwBBitMask = b;
    pf->dwRGBAlphaBitMask = a;
  };
  auto z = [&](uint32_t bits, uint32_t zmask, uint32_t sbits, uint32_t smask) {
    pf->dwFlags = DDPF_ZBUFFER | (sbits ? DDPF_STENCILBUFFER : 0);
    pf->dwRGBBitCount = bits;
    pf->dwRBitMask = sbits;
    pf->dwGBitMask = zmask;
    pf->dwBBitMask = smask;
  };
  switch (f) {
    case F::X8R8G8B8: rgb(32, 0xFF0000, 0xFF00, 0xFF, 0); break;
    case F::A8R8G8B8: rgb(32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000); break;
    case F::R8G8B8: rgb(24, 0xFF0000, 0xFF00, 0xFF, 0); break;
    case F::R5G6B5: rgb(16, 0xF800, 0x07E0, 0x001F, 0); break;
    case F::X1R5G5B5: rgb(16, 0x7C00, 0x03E0, 0x001F, 0); break;
    case F::A1R5G5B5: rgb(16, 0x7C00, 0x03E0, 0x001F, 0x8000); break;
    case F::A4R4G4B4: rgb(16, 0x0F00, 0x00F0, 0x000F, 0xF000); break;
    case F::X4R4G4B4: rgb(16, 0x0F00, 0x00F0, 0x000F, 0); break;
    case F::R3G3B2: rgb(8, 0xE0, 0x1C, 0x03, 0); break;
    case F::A8: pf->dwFlags = DDPF_ALPHA; pf->dwRGBBitCount = 8; break;
    case F::L8:
      pf->dwFlags = DDPF_LUMINANCE;
      pf->dwRGBBitCount = 8;
      pf->dwRBitMask = 0xFF;
      break;
    case F::A8L8:
      pf->dwFlags = DDPF_LUMINANCE | DDPF_ALPHAPIXELS;
      pf->dwRGBBitCount = 16;
      pf->dwRBitMask = 0xFF;
      pf->dwRGBAlphaBitMask = 0xFF00;
      break;
    case F::P8: pf->dwFlags = DDPF_PALETTEINDEXED8 | DDPF_RGB; pf->dwRGBBitCount = 8; break;
    case F::D16: z(16, 0xFFFF, 0, 0); break;
    case F::D15S1: z(16, 0xFFFE, 1, 0x0001); break;
    case F::D24X8: z(32, 0xFFFFFF00, 0, 0); break;
    case F::D24S8: z(32, 0xFFFFFF00, 8, 0x000000FF); break;
    case F::D32: z(32, 0xFFFFFFFF, 0, 0); break;
    case F::DXT1: case F::DXT2: case F::DXT3: case F::DXT4: case F::DXT5:
      pf->dwFlags = DDPF_FOURCC;
      pf->dwFourCC = 0x30545844 + (uint32_t(f) - uint32_t(F::DXT1) + 1);
      break;
    default: break;
  }
}

// ---- surfaces ------------------------------------------------------------------------

Surface::~Surface() {
  State& s = g_state;
  if (s.gfx) {
    if (tex && level == 0) s.gfx->DestroyTexture(tex);
    if (depth) s.gfx->DestroyDepth(depth);
  }
  if (shadow) {
    shadow.reset();
    g_proc->vm.Release(mem);
  } else if (mem) {
    HeapFreeProcess(mem);
  }
  if (s.backbuffer == this) s.backbuffer = nullptr;
}

void UploadLevel(Surface& s) {
  if (!s.tex || !s.mem) return;
  gfx::Rect r{0, 0, s.width, s.height};
  Gfx()->UpdateTexture(s.tex, s.level, r, GPtr(s.mem), s.pitch);
}

namespace {

uint32_t SurfaceVtable() {
  return com::Vtable(
      "ddraw.dll", "IDirectDrawSurface7",
      {"QueryInterface", "AddRef", "Release", "AddAttachedSurface",
       "AddOverlayDirtyRect", "Blt", "BltBatch", "BltFast",
       "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders",
       "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper",
       "GetColorKey", "GetDC", "GetFlipStatus", "GetOverlayPosition",
       "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize",
       "IsLost", "Lock", "ReleaseDC", "Restore", "SetClipper", "SetColorKey",
       "SetOverlayPosition", "SetPalette", "Unlock", "UpdateOverlay",
       "UpdateOverlayDisplay", "UpdateOverlayZOrder", "GetDDInterface",
       "PageLock", "PageUnlock", "SetSurfaceDesc", "SetPrivateData",
       "GetPrivateData", "FreePrivateData", "GetUniquenessValue",
       "ChangeUniquenessValue", "SetPriority", "GetPriority", "SetLOD",
       "GetLOD"});
}

bool OnGpu(const Surface& s) {
  return s.kind == Surface::Kind::BackBuffer || s.kind == Surface::Kind::Primary;
}

// Reads rect `r` of the back buffer or of the primary (the front buffer)
// into `dst` (r's top-left).
void ReadGpu(const Surface& s, const gfx::Rect& r, uint8_t* dst, uint32_t pitch) {
  if (s.kind == Surface::Kind::Primary && !g_state.front_from_gpu && !s.front_copy.empty()) {
    const uint32_t bpp = gfx::BitsPerPixel(s.fmt) / 8, n = uint32_t(r.right - r.left) * bpp;
    for (int y = r.top; y < r.bottom; y++)
      std::memcpy(dst + size_t(y - r.top) * pitch,
                  s.front_copy.data() + size_t(y) * s.pitch + size_t(r.left) * bpp, n);
    return;
  }
  if (s.kind == Surface::Kind::Primary)
    Gfx()->ReadBackFront(r, dst, pitch, s.fmt);
  else
    Gfx()->ReadBack(r, dst, pitch, s.fmt);
}

// Allocates guest memory for a surface of the surface's size/format. The
// back buffer's and the primary's get host pages of their own, which are
// protected while the game has them locked (lazy_mem.h).
void AllocMem(Surface& s) {
  s.pitch = gfx::RowPitch(s.fmt, uint32_t(s.width));
  s.pitch = (s.pitch + 3) & ~3u;
  uint32_t rows = gfx::RowCount(s.fmt, uint32_t(s.height));
  uint32_t size = std::max(1u, s.pitch * rows);
  if (!OnGpu(s)) {
    s.mem = HeapAllocProcess(size, true);
    return;
  }
  // Video memory goes on after a surface: games that draw a little past
  // its end (a mouse cursor at the screen's bottom edge, unclipped) write
  // into more of it on Windows. kOverrunRows rows of writable memory after
  // the image take such writes; they are never shown.
  constexpr uint32_t kOverrunRows = 64;
  size += s.pitch * kOverrunRows;
  uint32_t hp = uint32_t(g_proc->mem.host_page_size());
  size = (size + hp - 1) / hp * hp;
  s.mem = g_proc->vm.Reserve(0, size);
  if (!s.mem || !g_proc->vm.Commit(s.mem, size, Prot::RW))
    FatalF("ddraw: no guest memory for a %dx%d surface", s.width, s.height);
  const Surface* self = &s;
  s.shadow = std::make_unique<lazy::LazyImage>(
      g_proc->mem, s.mem, s.pitch, s.width, s.height, gfx::BitsPerPixel(s.fmt) / 8,
      [self](const lazy::LazyImage::Rect& r, uint8_t* dst, uint32_t pitch) {
        ReadGpu(*self, {r.left, r.top, r.right, r.bottom}, dst, pitch);
      });
  // Nothing has been pushed yet: the front buffer is as cleared, and so
  // is the copy.
  if (s.kind == Surface::Kind::Primary) s.front_copy.assign(size_t(s.pitch) * rows, 0);
}

std::shared_ptr<Surface> NewSurface(Surface::Kind kind, int w, int h,
                                    gfx::Format fmt, uint32_t caps,
                                    uint32_t caps2) {
  auto s = std::make_shared<Surface>();
  s->kind = kind;
  s->width = w;
  s->height = h;
  s->fmt = fmt;
  DDSURFACEDESC2& d = s->desc;
  d.dwSize = sizeof d;
  d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
  d.dwWidth = uint32_t(w);
  d.dwHeight = uint32_t(h);
  PixelFormatFromFormat(fmt, &d.ddpfPixelFormat);
  d.ddsCaps.dwCaps = caps;
  d.ddsCaps.dwCaps2 = caps2;
  d.lPitch = int32_t(gfx::RowPitch(fmt, uint32_t(w)));
  com::Publish(s, SurfaceVtable());
  return s;
}

Surface* Self(Ctx& c) {
  Surface* s = com::As<Surface>(hle::Arg(c, 0));
  if (!s) FatalF("IDirectDrawSurface7 call on unknown object %08X", uint32_t(hle::Arg(c, 0)));
  return s;
}

uint32_t CreateSurfaceImpl(const DDSURFACEDESC2& d, uint32_t* out) {
  uint32_t caps = d.ddsCaps.dwCaps, caps2 = d.ddsCaps.dwCaps2;
  int w = (d.dwFlags & DDSD_WIDTH) ? int(d.dwWidth) : 0;
  int h = (d.dwFlags & DDSD_HEIGHT) ? int(d.dwHeight) : 0;
  gfx::Format fmt = (d.dwFlags & DDSD_PIXELFORMAT) ? FormatFromPixelFormat(d.ddpfPixelFormat)
                                                    : gfx::Format::Unknown;
  RECOMP_TRACE("  CreateSurface caps %08X/%08X %dx%d fmt %u flags %08X\n", caps,
            caps2, w, h, unsigned(fmt), d.dwFlags);

  if (caps & DDSCAPS_PRIMARYSURFACE) {
    w = PrimaryWidth();
    h = PrimaryHeight();
    fmt = PrimaryFormat();
    Gfx();
    auto prim = NewSurface(Surface::Kind::Primary, w, h, fmt,
                           caps | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM |
                               DDSCAPS_FRONTBUFFER,
                           caps2);
    uint32_t backs = (d.dwFlags & DDSD_BACKBUFFERCOUNT) ? d.dwBackBufferCount : 0;
    if (backs) {
      prim->desc.dwFlags |= DDSD_BACKBUFFERCOUNT;
      prim->desc.dwBackBufferCount = backs;
      auto bb = NewSurface(Surface::Kind::BackBuffer, w, h, fmt,
                           (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER)) |
                               DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY |
                               DDSCAPS_LOCALVIDMEM,
                           caps2);
      prim->attached.push_back(bb);
      g_state.backbuffer = bb.get();
      if (backs > 1)
        WarnF("ddraw: %u back buffers requested; presenting through one\n", backs);
    } else {
      g_state.backbuffer = prim.get();  // the game always asks for one
    }
    *out = prim->guest;
    return DD_OK;
  }

  if (caps & DDSCAPS_ZBUFFER) {
    if (!w || !h) return DDERR_INVALIDPARAMS;
    if (fmt == gfx::Format::Unknown) {
      uint32_t bits = (d.dwFlags & DDSD_ZBUFFERBITDEPTH) ? d.dwMipMapCount : 16;
      fmt = bits == 16 ? gfx::Format::D16 : gfx::Format::D24X8;
    }
    auto z = NewSurface(Surface::Kind::ZBuffer, w, h, fmt,
                        caps | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM, caps2);
    z->depth = Gfx()->CreateDepth(w, h, fmt);
    if (!z->depth) {
      com::Release(z->guest);
      return DDERR_OUTOFVIDEOMEMORY;
    }
    *out = z->guest;
    return DD_OK;
  }

  if (fmt == gfx::Format::Unknown) {
    if (d.dwFlags & DDSD_PIXELFORMAT) {
      WarnF("ddraw: unsupported pixel format flags %08X bits %u\n",
            d.ddpfPixelFormat.dwFlags, d.ddpfPixelFormat.dwRGBBitCount);
      return DDERR_INVALIDPIXELFORMAT;
    }
    fmt = PrimaryFormat();
  }
  if (!w || !h) return DDERR_INVALIDPARAMS;

  if (caps & DDSCAPS_TEXTURE) {
    int levels = 1;
    if (caps & DDSCAPS_MIPMAP) {
      if (d.dwFlags & DDSD_MIPMAPCOUNT) {
        levels = int(d.dwMipMapCount);
      } else {
        for (int m = std::max(w, h); m > 1; m >>= 1) levels++;
      }
    }
    uint32_t tcaps = caps | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    if (caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE))
      tcaps &= ~(DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
    // DDSCAPS_3DDEVICE: the game renders into it (shadow textures).
    gfx::TextureId tex =
        Gfx()->CreateTexture(w, h, levels, fmt, (caps & DDSCAPS_3DDEVICE) != 0);
    if (!tex) return DDERR_OUTOFVIDEOMEMORY;
    std::shared_ptr<Surface> top, prev;
    for (int l = 0; l < levels; l++) {
      int lw = std::max(1, w >> l), lh = std::max(1, h >> l);
      auto s = NewSurface(Surface::Kind::Texture, lw, lh, fmt, tcaps,
                          caps2 | (l ? DDSCAPS2_MIPMAPSUBLEVEL : 0));
      s->tex = tex;
      s->level = l;
      AllocMem(*s);
      s->desc.lPitch = int32_t(s->pitch);
      if (l == 0 && levels > 1) {
        s->desc.dwFlags |= DDSD_MIPMAPCOUNT;
        s->desc.dwMipMapCount = uint32_t(levels);
      }
      if (d.dwFlags & DDSD_CKSRCBLT) {
        s->has_ck_src = true;
        s->ck_src = d.ddckCKSrcBlt;
      }
      if (prev) prev->attached.push_back(s);
      if (!top) top = s;
      prev = s;
    }
    *out = top->guest;
    return DD_OK;
  }

  // Offscreen plain / system memory surfaces: guest memory only.
  auto s = NewSurface(Surface::Kind::Offscreen, w, h, fmt,
                      caps | ((caps & DDSCAPS_VIDEOMEMORY) ? 0 : DDSCAPS_SYSTEMMEMORY),
                      caps2);
  AllocMem(*s);
  s->desc.lPitch = int32_t(s->pitch);
  if (d.dwFlags & DDSD_CKSRCBLT) {
    s->has_ck_src = true;
    s->ck_src = d.ddckCKSrcBlt;
  }
  *out = s->guest;
  return DD_OK;
}

// Fills `dst` rectangle of a memory surface with a raw pixel value.
void FillMem(Surface& s, const guest::RECT& r, uint32_t raw) {
  uint32_t bpp = gfx::BitsPerPixel(s.fmt) / 8;
  for (int y = r.top; y < r.bottom; y++)
    for (int x = r.left; x < r.right; x++)
      gfx::PutRawPixel(GPtr(s.mem + uint32_t(y) * s.pitch + uint32_t(x) * bpp), s.fmt, raw);
}

guest::RECT FullRect(const Surface& s) { return {0, 0, s.width, s.height}; }

guest::RECT ReadRect(uint32_t va, const Surface& s) {
  return va ? *G<guest::RECT>(va) : FullRect(s);
}

// Memory-to-memory blit with optional stretch (nearest) and source key.
void BlitMem(Surface& dst, const guest::RECT& dr, Surface& src,
             const guest::RECT& sr, bool key, uint32_t keyval) {
  uint32_t dbpp = gfx::BitsPerPixel(dst.fmt) / 8, sbpp = gfx::BitsPerPixel(src.fmt) / 8;
  int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
  int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
  if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
  bool same = dst.fmt == src.fmt;
  for (int y = 0; y < dh; y++) {
    int sy = sr.top + y * sh / dh;
    for (int x = 0; x < dw; x++) {
      int sx = sr.left + x * sw / dw;
      const uint8_t* sp = GPtr(src.mem + uint32_t(sy) * src.pitch + uint32_t(sx) * sbpp);
      if (key && gfx::RawPixel(sp, src.fmt) == keyval) continue;
      uint8_t* dp = GPtr(dst.mem + uint32_t(dr.top + y) * dst.pitch + uint32_t(dr.left + x) * dbpp);
      if (same)
        std::memcpy(dp, sp, sbpp);
      else
        gfx::EncodePixel(dp, dst.fmt, gfx::DecodePixel(sp, src.fmt));
    }
  }
}

// Makes rect `r` of the back buffer's or the primary's guest memory
// accessible to host code (it may be protected for a lazy read-back).
void Settle(Surface& s, const guest::RECT& r) {
  if (!s.mem) AllocMem(s);
  s.shadow->Settle({r.left, r.top, r.right, r.bottom});
}

// Brings rect `r` of the back buffer's or the primary's (front buffer's)
// pixels into its guest memory, for a Blt that reads them.
void Pull(Surface& s, const guest::RECT& r) {
  Settle(s, r);
  gfx::Rect c{std::max(r.left, 0), std::max(r.top, 0), std::min(r.right, s.width),
              std::min(r.bottom, s.height)};
  if (c.right <= c.left || c.bottom <= c.top) return;
  uint32_t off = uint32_t(c.top) * s.pitch + uint32_t(c.left) * (gfx::BitsPerPixel(s.fmt) / 8);
  ReadGpu(s, c, GPtr(s.mem + off), s.pitch);
}

// Writes rows/rect `r` of the primary's guest memory to the front buffer.
// It is shown at once only when the game is not presenting frames anyway
// (each frame replaces the front buffer, cursor included).
void PushFront(Surface& s, const gfx::Rect& r) {
  if (r.right <= r.left || r.bottom <= r.top) return;
  RECOMP_TRACE("  PushFront (%d,%d,%d,%d)\n", r.left, r.top, r.right, r.bottom);
  Gfx()->BlitFront(GPtr(s.mem), s.pitch, s.fmt, r, r);
  if (!s.front_copy.empty()) {
    const uint32_t bpp = gfx::BitsPerPixel(s.fmt) / 8, n = uint32_t(r.right - r.left) * bpp;
    for (int y = r.top; y < r.bottom; y++) {
      const size_t off = size_t(y) * s.pitch + size_t(r.left) * bpp;
      std::memcpy(s.front_copy.data() + off, GPtr(s.mem + uint32_t(off)), n);
    }
    // A push of the whole image makes the copy the front buffer again.
    if (r.left <= 0 && r.top <= 0 && r.right >= s.width && r.bottom >= s.height)
      g_state.front_from_gpu = false;
  }
  if (SDL_GetTicks() - g_last_present_ms > 50) {
    const Uint64 now = SDL_GetTicks();
    if (!g_front_pending.exchange(true)) g_front_pending_ms = now;
    else if (now - g_front_pending_ms > kFrontStaleMs) PresentPendingFront(false);
  }
  // <ID>_SHOT_MS=n (with SHOT_DIR): the primary's pixels as a BMP at most
  // every n ms (primary_NNNNN.bmp), and at most every kLatestMs as
  // primary_latest.bmp (replaced atomically, so a reader never sees a
  // partial file), for games that draw straight to the primary (their
  // frames never go through PresentFrame's SHOT_EVERY).
  constexpr Uint64 kLatestMs = 200;
  static const int shot_ms = [] {
    const char* e = Env("SHOT_MS");
    return e ? std::atoi(e) : 0;
  }();
  static Uint64 last_shot = 0, last_latest = 0;
  static int shot = 0;
  const Uint64 now = SDL_GetTicks();
  const bool numbered = now - last_shot >= Uint64(shot_ms);
  if (shot_ms > 0 && (numbered || now - last_latest >= kLatestMs) && (s.fmt == gfx::Format::R5G6B5 || s.fmt == gfx::Format::X8R8G8B8)) {
    // From the front buffer: the primary's guest pages may be protected
    // (lazy_mem.h) while this runs.
    std::vector<uint8_t> px(size_t(s.pitch) * uint32_t(s.height));
    Gfx()->ReadBackFront({0, 0, s.width, s.height}, px.data(), s.pitch, s.fmt);
    const SDL_PixelFormat pf =
        s.fmt == gfx::Format::R5G6B5 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_XRGB8888;
    if (SDL_Surface* surf =
            SDL_CreateSurfaceFrom(s.width, s.height, pf, px.data(), int(s.pitch))) {
      const char* dir = Env("SHOT_DIR");
      char path[512];
      char tmp[520];
      std::snprintf(path, sizeof path, "%s/primary_latest.bmp", dir ? dir : ".");
      std::snprintf(tmp, sizeof tmp, "%s.tmp", path);
      if (SDL_SaveBMP(surf, tmp)) std::rename(tmp, path);
      last_latest = now;
      if (numbered) {
        last_shot = now;
        std::snprintf(path, sizeof path, "%s/primary_%05d.bmp", dir ? dir : ".", shot++);
        SDL_SaveBMP(surf, path);
      }
      SDL_DestroySurface(surf);
    }
  }
}

// `fill_raw` is DDBLTFX's dwFillColor/dwFillDepth union.
uint32_t BltImpl(Surface& dst, const guest::RECT& dr, Surface* src,
                 const guest::RECT& sr, bool key, uint32_t keyval, bool fill,
                 bool depth_fill, uint32_t fill_raw) {
  if (depth_fill) {
    if (!dst.depth) return DDERR_INVALIDPARAMS;
    // Only full clears are expected; the gfx clear acts on the bound Z.
    float z = dst.fmt == gfx::Format::D16 ? float(fill_raw) / 65535.0f
                                          : float(fill_raw & 0xFFFFFF) / 16777215.0f;
    gfx::Device* g = Gfx();
    g->SetRenderTarget(0, dst.depth);
    g->Clear(0, nullptr, 2 /* ZBUFFER */, 0, z, 0);
    if (g_state.backbuffer)
      g->SetRenderTarget(0, g_state.backbuffer->zbuffer ? g_state.backbuffer->zbuffer->depth : 0);
    return DD_OK;
  }
  if (fill && dst.kind == Surface::Kind::Primary) {
    Settle(dst, dr);
    FillMem(dst, dr, fill_raw);
    PushFront(dst, {dr.left, dr.top, dr.right, dr.bottom});
    return DD_OK;
  }
  if (fill) {
    bool target_tex = dst.kind == Surface::Kind::Texture &&
                      (dst.desc.ddsCaps.dwCaps & DDSCAPS_3DDEVICE);
    if (dst.kind == Surface::Kind::BackBuffer || target_tex) {
      uint32_t px[1];
      gfx::PutRawPixel(reinterpret_cast<uint8_t*>(px), dst.fmt, fill_raw);
      uint32_t argb = gfx::DecodePixel(reinterpret_cast<uint8_t*>(px), dst.fmt);
      gfx::Rect r{dr.left, dr.top, dr.right, dr.bottom};
      if (target_tex)
        Gfx()->FillTexture(dst.tex, r, argb);
      else
        Gfx()->Clear(1, &r, 1 /* TARGET */, argb, 1.0f, 0);
      return DD_OK;
    }
    if (!dst.mem) return DDERR_UNSUPPORTED;
    FillMem(dst, dr, fill_raw);
    if (dst.kind == Surface::Kind::Texture) UploadLevel(dst);
    return DD_OK;
  }
  if (!src) return DDERR_INVALIDPARAMS;
  // Back buffer -> primary is how the game presents (instead of Flip):
  // that is a swap-chain present, not a pixel copy.
  if (dst.kind == Surface::Kind::Primary && src->kind == Surface::Kind::BackBuffer) {
    PresentPrimary();
    return DD_OK;
  }
  if (OnGpu(*src)) Pull(*src, sr);
  if (!src->mem) return DDERR_UNSUPPORTED;
  if (dst.kind == Surface::Kind::Primary) {
    // Keyed or overlapping blits keep some destination pixels.
    if (key || src == &dst)
      Pull(dst, dr);
    else
      Settle(dst, dr);
    BlitMem(dst, dr, *src, sr, key, keyval);
    PushFront(dst, {dr.left, dr.top, dr.right, dr.bottom});
    return DD_OK;
  }
  if (dst.kind == Surface::Kind::BackBuffer) {
    gfx::Rect s{sr.left, sr.top, sr.right, sr.bottom};
    gfx::Rect d{dr.left, dr.top, dr.right, dr.bottom};
    Gfx()->Blit(GPtr(src->mem), src->pitch, src->fmt, s, d, key, keyval);
    return DD_OK;
  }
  if (!dst.mem) return DDERR_UNSUPPORTED;
  if (gfx::IsCompressed(dst.fmt) || gfx::IsCompressed(src->fmt)) {
    if (dst.fmt != src->fmt || sr.left || sr.top || dr.left || dr.top ||
        sr.right != src->width || dr.right != dst.width)
      return DDERR_UNSUPPORTED;
    std::memcpy(GPtr(dst.mem), GPtr(src->mem),
                dst.pitch * gfx::RowCount(dst.fmt, uint32_t(dst.height)));
  } else {
    BlitMem(dst, dr, *src, sr, key, keyval);
  }
  if (dst.kind == Surface::Kind::Texture) UploadLevel(dst);
  return DD_OK;
}

}  // namespace

}  // namespace recomp::ddraw

namespace recomp::hle {
namespace {

using namespace ddraw;

// ---- exports ----------------------------------------------------------------------

uint32_t DirectDrawVtable() {
  return com::Vtable(
      "ddraw.dll", "IDirectDraw7",
      {"QueryInterface", "AddRef", "Release", "Compact", "CreateClipper",
       "CreatePalette", "CreateSurface", "DuplicateSurface",
       "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps",
       "GetDisplayMode", "GetFourCCCodes", "GetGDISurface",
       "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus",
       "Initialize", "RestoreDisplayMode", "SetCooperativeLevel",
       "SetDisplayMode", "WaitForVerticalBlank", "GetAvailableVidMem",
       "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel",
       "GetDeviceIdentifier", "StartModeTest", "EvaluateMode"});
}

struct DirectDraw : com::Object {};

// DirectDrawEnumerateExA(callback, context, flags): one adapter, the
// primary display (NULL GUID). Callback: BOOL(GUID*, LPSTR desc, LPSTR
// name, LPVOID ctx, HMONITOR).
HLE("ddraw.dll", DirectDrawEnumerateExA, 3) {
  uint32_t cb = Arg(c, 0), ctx = Arg(c, 1);
  uint32_t strs = HeapAllocProcess(64, true);
  PutStr(strs, "Primary Display Driver");
  PutStr(strs + 32, "display");
  CallGuest(c, cb, {0, strs, strs + 32, ctx, 0});
  HeapFreeProcess(strs);
  return DD_OK;
}

HLE("ddraw.dll", DirectDrawCreateEx, 4) {
  uint32_t guid = Arg(c, 0), out = Arg(c, 1), iid = Arg(c, 2);
  if (guid > 2 && Rd32(guid)) {  // only the primary display (NULL GUID)
    Wr32(out, 0);
    return DDERR_INVALIDPARAMS;
  }
  if (!com::GuidEq(iid, kIID_IDirectDraw7)) return com::kE_NOINTERFACE;
  auto dd = std::make_shared<DirectDraw>();
  Wr32(out, com::Publish(dd, DirectDrawVtable()));
  return DD_OK;
}

// ---- IDirectDraw7 ----------------------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirectDraw7::QueryInterface", DD7_QueryInterface, 3) {
  uint32_t self = Arg(c, 0), iid = Arg(c, 1), out = Arg(c, 2);
  if (com::GuidEq(iid, kIID_IDirectDraw7)) {
    com::AddRef(self);
    Wr32(out, self);
    return DD_OK;
  }
  if (com::GuidEq(iid, kIID_IDirect3D7)) {
    Wr32(out, D3D7For(self));
    return DD_OK;
  }
  guest::GUID g = com::ReadGuid(iid);
  WarnF("IDirectDraw7::QueryInterface({%08X-...}): no interface\n", g.Data1);
  Wr32(out, 0);
  return com::kE_NOINTERFACE;
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::AddRef", DD7_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirectDraw7::Release", DD7_Release, 1) { return com::Release(Arg(c, 0)); }

HLE_NAMED("ddraw.dll", "IDirectDraw7::SetCooperativeLevel", DD7_SetCooperativeLevel, 3) {
  State& s = St();
  if (Arg(c, 1)) s.hwnd = Arg(c, 1);
  s.coop_flags = Arg(c, 2);
  RECOMP_TRACE("  SetCooperativeLevel(hwnd %X, flags %X)\n", uint32_t(Arg(c, 1)), uint32_t(Arg(c, 2)));
  return DD_OK;
}

void FillModeDesc(DDSURFACEDESC2* d, int w, int h, int bpp) {
  std::memset(d, 0, sizeof *d);
  d->dwSize = sizeof *d;
  d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
  d->dwWidth = uint32_t(w);
  d->dwHeight = uint32_t(h);
  d->lPitch = w * bpp / 8;
  d->dwMipMapCount = 60;  // dwRefreshRate
  PixelFormatFromFormat(bpp == 16 ? gfx::Format::R5G6B5 : gfx::Format::X8R8G8B8,
                        &d->ddpfPixelFormat);
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::EnumDisplayModes", DD7_EnumDisplayModes, 5) {
  uint32_t cb = Arg(c, 4), ctx = Arg(c, 3);
  EnsureSdlVideo();
  uint32_t d = HeapAllocProcess(sizeof(DDSURFACEDESC2), true);
  for (int bpp : {16, 32})
    for (const ModeSize& m : DisplayModeSizes()) {
      FillModeDesc(G<DDSURFACEDESC2>(d), m.w, m.h, bpp);
      if (!CallGuest(c, cb, {d, ctx})) goto done;
    }
done:
  HeapFreeProcess(d);
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::GetDisplayMode", DD7_GetDisplayMode, 2) {
  State& s = St();
  EnsureSdlVideo();
  int w = s.mode_w ? s.mode_w : g_display_w, h = s.mode_h ? s.mode_h : g_display_h;
  FillModeDesc(G<DDSURFACEDESC2>(Arg(c, 1)), w, h, s.mode_bpp ? s.mode_bpp : 32);
  return DD_OK;
}

void SetMode(int w, int h, int bpp) {
  State& s = St();
  RECOMP_TRACE("  SetDisplayMode(%d x %d x %d)\n", w, h, bpp);
  s.mode_w = w;
  s.mode_h = h;
  s.mode_bpp = bpp;
  // The "mode" is the window's client size; the desktop is left alone.
  SetExclusiveMode(s.hwnd, w, h);
  if (s.gfx) s.gfx->ResetSwapChain(w, h, s.vsync);
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::SetDisplayMode", DD7_SetDisplayMode, 6) {
  SetMode(int(Arg(c, 1)), int(Arg(c, 2)), int(Arg(c, 3)));
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::RestoreDisplayMode", DD7_RestoreDisplayMode, 1) { return DD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw7::TestCooperativeLevel", DD7_TestCooperativeLevel, 1) { return DD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw7::RestoreAllSurfaces", DD7_RestoreAllSurfaces, 1) { return DD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw7::FlipToGDISurface", DD7_FlipToGDISurface, 1) { return DD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDraw7::Compact", DD7_Compact, 1) { return DD_OK; }

HLE_NAMED("ddraw.dll", "IDirectDraw7::WaitForVerticalBlank", DD7_WaitForVerticalBlank, 3) { return DD_OK; }

HLE_NAMED("ddraw.dll", "IDirectDraw7::GetMonitorFrequency", DD7_GetMonitorFrequency, 2) {
  Wr32(Arg(c, 1), 60);
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::GetVerticalBlankStatus", DD7_GetVerticalBlankStatus, 2) {
  Wr32(Arg(c, 1), 1);
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::GetScanLine", DD7_GetScanLine, 2) {
  Wr32(Arg(c, 1), 0);
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::GetCaps", DD7_GetCaps, 3) {
  for (int i = 1; i <= 2; i++) {
    uint32_t va = Arg(c, i);
    if (!va) continue;
    uint32_t size = Rd32(va);
    guest::DDCAPS_DX7 k{};
    k.dwSize = size;
    if (i == 1) {  // driver caps; HEL caps stay mostly empty
      k.dwCaps = 0x1 /*3D*/ | 0x40 /*BLT*/ | 0x200 /*BLTSTRETCH*/ |
                 0x400000 /*COLORKEY*/ | 0x800000 /*ALPHA*/ |
                 0x4000000 /*BLTCOLORFILL*/ | 0x80000000 /*CANBLTSYSMEM*/ |
                 0x40000000 /*BLTDEPTHFILL*/;
      k.dwCaps2 = 0x1000 /*WIDESURFACES*/ | 0x20000 /*PRIMARYGAMMA*/ |
                  0x80000 /*CANRENDERWINDOWED*/ | 0x8000000 /*FLIPNOVSYNC*/ |
                  0x200 /*NOPAGELOCKREQUIRED*/;
      k.dwCKeyCaps = 0x200;  // DDCKEYCAPS_SRCBLT
      k.dwZBufferBitDepths = 0x400 | 0x200 | 0x100;  // 16, 24, 32
      k.dwVidMemTotal = VidMemTotal();
      k.dwVidMemFree = VidMemFree();
      k.ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER |
                         DDSCAPS_FLIP | DDSCAPS_PRIMARYSURFACE |
                         DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY |
                         DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM |
                         DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY |
                         DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
      k.ddsOldCaps.dwCaps = k.ddsCaps.dwCaps;
    }
    std::memcpy(GPtr(va), &k, std::min<uint32_t>(size ? size : sizeof k, sizeof k));
    Wr32(va, size);
  }
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::GetAvailableVidMem", DD7_GetAvailableVidMem, 4) {
  if (Arg(c, 2)) Wr32(Arg(c, 2), VidMemTotal());
  if (Arg(c, 3)) Wr32(Arg(c, 3), VidMemFree());
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::GetFourCCCodes", DD7_GetFourCCCodes, 3) {
  uint32_t num = Arg(c, 1), codes = Arg(c, 2);
  const uint32_t k[] = {0x31545844, 0x32545844, 0x33545844, 0x34545844, 0x35545844};
  if (codes)
    for (uint32_t i = 0; i < std::min<uint32_t>(Rd32(num), 5); i++) Wr32(codes + 4 * i, k[i]);
  Wr32(num, 5);
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::GetDeviceIdentifier", DD7_GetDeviceIdentifier, 3) {
  guest::DDDEVICEIDENTIFIER2 id{};
  std::strcpy(id.szDriver, "nv4_disp.dll");
  std::strcpy(id.szDescription, "NVIDIA GeForce3");
  id.liDriverVersionHigh = 0x00060000 | 14;  // 6.14.10.xxxx style
  id.liDriverVersionLow = 0x000A0000 | 2960;
  id.dwVendorId = 0x10DE;
  id.dwDeviceId = 0x0200;
  id.guidDeviceIdentifier = {0xD7B71E3E, 0x4340, 0x11CF, {0x83, 0x63, 0x23, 0x01, 0xA4, 0xC2, 0xC3, 0x5B}};
  id.dwWHQLLevel = 1;
  std::memcpy(GPtr(Arg(c, 1)), &id, sizeof id);
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDraw7::CreateSurface", DD7_CreateSurface, 4) {
  uint32_t out = 0;
  uint32_t hr = CreateSurfaceImpl(*G<DDSURFACEDESC2>(Arg(c, 1)), &out);
  Wr32(Arg(c, 2), out);
  return hr;
}

// ---- IDirectDrawSurface7 ----------------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::QueryInterface", DDS7_QueryInterface, 3) {
  uint32_t self = Arg(c, 0), iid = Arg(c, 1), out = Arg(c, 2);
  if (com::GuidEq(iid, kIID_IDirectDrawSurface7)) {
    com::AddRef(self);
    Wr32(out, self);
    return DD_OK;
  }
  if (com::GuidEq(iid, kIID_IDirectDrawGammaControl)) {
    struct Gamma : com::Object {};
    auto g = std::make_shared<Gamma>();
    Wr32(out, com::Publish(g, com::Vtable("ddraw.dll", "IDirectDrawGammaControl",
                                          {"QueryInterface", "AddRef", "Release",
                                           "GetGammaRamp", "SetGammaRamp"})));
    return DD_OK;
  }
  guest::GUID g = com::ReadGuid(iid);
  WarnF("IDirectDrawSurface7::QueryInterface({%08X-...}): no interface\n", g.Data1);
  Wr32(out, 0);
  return com::kE_NOINTERFACE;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::AddRef", DDS7_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::Release", DDS7_Release, 1) { return com::Release(Arg(c, 0)); }

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetSurfaceDesc", DDS7_GetSurfaceDesc, 2) {
  Surface* s = Self(c);
  DDSURFACEDESC2 d = s->desc;
  d.lpSurface = 0;
  std::memcpy(GPtr(Arg(c, 1)), &d, sizeof d);
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetPixelFormat", DDS7_GetPixelFormat, 2) {
  std::memcpy(GPtr(Arg(c, 1)), &Self(c)->desc.ddpfPixelFormat, sizeof(DDPIXELFORMAT));
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetCaps", DDS7_GetCaps, 2) {
  std::memcpy(GPtr(Arg(c, 1)), &Self(c)->desc.ddsCaps, sizeof(guest::DDSCAPS2));
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetAttachedSurface", DDS7_GetAttachedSurface, 3) {
  Surface* s = Self(c);
  const guest::DDSCAPS2& want = *G<guest::DDSCAPS2>(Arg(c, 1));
  for (auto& a : s->attached) {
    if ((a->desc.ddsCaps.dwCaps & want.dwCaps) == want.dwCaps) {
      com::AddRef(a->guest);
      Wr32(Arg(c, 2), a->guest);
      return DD_OK;
    }
  }
  Wr32(Arg(c, 2), 0);
  return DDERR_NOTFOUND;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::AddAttachedSurface", DDS7_AddAttachedSurface, 2) {
  Surface* s = Self(c);
  auto a = std::dynamic_pointer_cast<Surface>(com::Lookup(Arg(c, 1)));
  if (!a) return DDERR_INVALIDPARAMS;
  if (a->kind != Surface::Kind::ZBuffer) {
    WarnF("ddraw: AddAttachedSurface of a non-Z surface: unsupported\n");
    return DDERR_UNSUPPORTED;
  }
  com::AddRef(a->guest);
  s->attached.push_back(a);
  s->zbuffer = a.get();
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::DeleteAttachedSurface", DDS7_DeleteAttachedSurface, 3) {
  Surface* s = Self(c);
  uint32_t which = Arg(c, 2);
  for (auto it = s->attached.begin(); it != s->attached.end(); ++it) {
    if (!which || (*it)->guest == which) {
      if (s->zbuffer == it->get()) s->zbuffer = nullptr;
      uint32_t g = (*it)->guest;
      s->attached.erase(it);
      com::Release(g);
      return DD_OK;
    }
  }
  return DDERR_NOTFOUND;
}

// Locks rect `rect` (a guest RECT*, 0 for all) of s; *out is the surface
// description with lpSurface and lPitch set.
uint32_t LockImpl(Surface* s, uint32_t rect, uint32_t flags, DDSURFACEDESC2* out) {
  if (s->kind == Surface::Kind::ZBuffer) {
    WarnF("ddraw: Lock of a Z buffer is not supported\n");
    return DDERR_UNSUPPORTED;
  }
  if (!s->mem) AllocMem(*s);
  if (OnGpu(*s)) {
    // Nothing is read back yet: the pixels come in when the game touches
    // them. DDLOCK_READONLY (0x10): nothing goes back on Unlock.
    // DDLOCK_WRITEONLY still reads what is touched, so that bytes the game
    // leaves alone in a row it writes go back unchanged.
    lazy::LazyImage::Rect r{0, 0, s->width, s->height};
    if (rect) {
      const guest::RECT& g = *G<guest::RECT>(rect);
      r = {g.left, g.top, g.right, g.bottom};
    }
    s->shadow->Begin(r, flags & 0x10);
  }
  s->locked = true;
  DDSURFACEDESC2 d = s->desc;
  d.dwFlags |= DDSD_LPSURFACE | DDSD_PITCH;
  d.lPitch = int32_t(s->pitch);
  uint32_t off = 0;
  if (rect) {
    const guest::RECT& r = *G<guest::RECT>(rect);
    uint32_t bpp = gfx::BitsPerPixel(s->fmt);
    off = uint32_t(r.top) * s->pitch + uint32_t(r.left) * bpp / 8;
  }
  d.lpSurface = s->mem + off;
  *out = d;
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::Lock", DDS7_Lock, 5) {
  DDSURFACEDESC2 d;
  uint32_t hr = LockImpl(Self(c), Arg(c, 1), Arg(c, 3), &d);
  if (hr == DD_OK) std::memcpy(GPtr(Arg(c, 2)), &d, sizeof d);
  return hr;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::Unlock", DDS7_Unlock, 2) {
  Surface* s = Self(c);
  if (!s->locked) return DDERR_NOTLOCKED;
  s->locked = false;
  if (s->kind == Surface::Kind::Texture) {
    UploadLevel(*s);
  } else if (OnGpu(*s)) {
    // Write the rows the game edited back.
    s->shadow->End([s](const lazy::LazyImage::Rect& b) {
      gfx::Rect r{b.left, b.top, b.right, b.bottom};
      if (s->kind == Surface::Kind::Primary)
        PushFront(*s, r);
      else
        Gfx()->Blit(GPtr(s->mem), s->pitch, s->fmt, r, r, false, 0);
    });
  }
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::Blt", DDS7_Blt, 6) {
  Surface* dst = Self(c);
  uint32_t drect = Arg(c, 1), srcp = Arg(c, 2), srect = Arg(c, 3),
           flags = Arg(c, 4), fx = Arg(c, 5);
  Surface* src = srcp ? com::As<Surface>(srcp) : nullptr;
  bool key = false;
  uint32_t keyval = 0;
  if ((flags & 0x8000) && src && src->has_ck_src) {  // DDBLT_KEYSRC
    key = true;
    keyval = src->ck_src.dwColorSpaceLowValue;
  }
  if ((flags & 0x10000) && fx) {  // DDBLT_KEYSRCOVERRIDE
    key = true;
    keyval = G<guest::DDBLTFX>(fx)->ddckSrcColorkey.dwColorSpaceLowValue;
  }
  bool fill = flags & 0x400;          // DDBLT_COLORFILL
  bool zfill = flags & 0x2000000;     // DDBLT_DEPTHFILL
  uint32_t fillv = fx ? G<guest::DDBLTFX>(fx)->dwFillColor : 0;
  guest::RECT sr = src ? ReadRect(srect, *src) : guest::RECT{};
  return BltImpl(*dst, ReadRect(drect, *dst), src, sr, key, keyval, fill, zfill, fillv);
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::BltFast", DDS7_BltFast, 6) {
  Surface* dst = Self(c);
  int32_t x = int32_t(Arg(c, 1)), y = int32_t(Arg(c, 2));
  Surface* src = com::As<Surface>(Arg(c, 3));
  uint32_t srect = Arg(c, 4), flags = Arg(c, 5);
  if (!src) return DDERR_INVALIDPARAMS;
  guest::RECT sr = ReadRect(srect, *src);
  guest::RECT dr{x, y, x + (sr.right - sr.left), y + (sr.bottom - sr.top)};
  bool key = (flags & 1) && src->has_ck_src;  // DDBLTFAST_SRCCOLORKEY
  return BltImpl(*dst, dr, src, sr, key, src->ck_src.dwColorSpaceLowValue,
                 false, false, 0);
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::Flip", DDS7_Flip, 3) {
  PresentPrimary();
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::SetColorKey", DDS7_SetColorKey, 3) {
  Surface* s = Self(c);
  uint32_t flags = Arg(c, 1), ck = Arg(c, 2);
  if (flags & 0x8) {  // DDCKEY_SRCBLT
    s->has_ck_src = ck != 0;
    if (ck) s->ck_src = *G<guest::DDCOLORKEY>(ck);
    if (ck) {
      s->desc.dwFlags |= DDSD_CKSRCBLT;
      s->desc.ddckCKSrcBlt = s->ck_src;
    } else {
      s->desc.dwFlags &= ~DDSD_CKSRCBLT;
    }
    return DD_OK;
  }
  WarnF("ddraw: SetColorKey flags %X unsupported\n", flags);
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetColorKey", DDS7_GetColorKey, 3) {
  Surface* s = Self(c);
  if ((Arg(c, 1) & 0x8) && s->has_ck_src) {
    *G<guest::DDCOLORKEY>(Arg(c, 2)) = s->ck_src;
    return DD_OK;
  }
  return DDERR_NOCOLORKEY;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::IsLost", DDS7_IsLost, 1) { return DD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::Restore", DDS7_Restore, 1) { return DD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::PageLock", DDS7_PageLock, 2) { return DD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::PageUnlock", DDS7_PageUnlock, 2) { return DD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetBltStatus", DDS7_GetBltStatus, 2) { return DD_OK; }
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetFlipStatus", DDS7_GetFlipStatus, 2) { return DD_OK; }

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::SetPriority", DDS7_SetPriority, 2) {
  Self(c)->priority = Arg(c, 1);
  return DD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetPriority", DDS7_GetPriority, 2) {
  Wr32(Arg(c, 1), Self(c)->priority);
  return DD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::SetLOD", DDS7_SetLOD, 2) {
  Self(c)->lod = Arg(c, 1);
  return DD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetLOD", DDS7_GetLOD, 2) {
  Wr32(Arg(c, 1), Self(c)->lod);
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::GetUniquenessValue", DDS7_GetUniquenessValue, 2) {
  Wr32(Arg(c, 1), 1);
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDrawSurface7::SetClipper", DDS7_SetClipper, 2) { return DD_OK; }

// ---- IDirectDrawGammaControl -------------------------------------------------------
// The ramp is stored; applying it needs a post-process pass in the backend
// (SDL3 has no window gamma ramps). TODO(gfx): apply.

uint16_t g_gamma[3][256];
bool g_gamma_init = false;

void InitGamma() {
  if (g_gamma_init) return;
  for (int ch = 0; ch < 3; ch++)
    for (int i = 0; i < 256; i++) g_gamma[ch][i] = uint16_t(i * 257);
  g_gamma_init = true;
}

HLE_NAMED("ddraw.dll", "IDirectDrawGammaControl::AddRef", DDG_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirectDrawGammaControl::Release", DDG_Release, 1) { return com::Release(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirectDrawGammaControl::GetGammaRamp", DDG_GetGammaRamp, 3) {
  InitGamma();
  std::memcpy(GPtr(Arg(c, 2)), g_gamma, sizeof g_gamma);
  return DD_OK;
}
HLE_NAMED("ddraw.dll", "IDirectDrawGammaControl::SetGammaRamp", DDG_SetGammaRamp, 3) {
  InitGamma();
  std::memcpy(g_gamma, GPtr(Arg(c, 2)), sizeof g_gamma);
  return DD_OK;
}


// ---- DirectDraw 1: DirectDrawCreate, IDirectDraw, IDirectDrawSurface ------------
// The first interfaces, for games written against DirectX 3-5. They are the
// same objects as the version 7 ones: the methods with the same arguments
// share the version 7 handlers (registered again under the version 1
// names); the rest convert DDSURFACEDESC / DDSCAPS (the shorter version 1
// structures) and the version 1 argument lists.

// Registers the handler of `target` (an HLE_NAMED ident) under another
// export name.
#define DD1_ALIAS(export_name, ident, nargs, target)                            \
  static ::recomp::hle::Registrar RECOMP_HLE_CAT(reg_, ident){                 \
      "ddraw.dll", export_name, nargs, &RECOMP_HLE_CAT(hle_, target), false}

constexpr guest::GUID kIID_IDirectDraw = {0x6C14DB80, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
constexpr guest::GUID kIID_IDirectDrawSurface = {0x6C14DB81, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

uint32_t DirectDraw1Vtable() {
  return com::Vtable(
      "ddraw.dll", "IDirectDraw",
      {"QueryInterface", "AddRef", "Release", "Compact", "CreateClipper",
       "CreatePalette", "CreateSurface", "DuplicateSurface",
       "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps",
       "GetDisplayMode", "GetFourCCCodes", "GetGDISurface",
       "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus",
       "Initialize", "RestoreDisplayMode", "SetCooperativeLevel",
       "SetDisplayMode", "WaitForVerticalBlank"});
}

uint32_t Surface1Vtable() {
  return com::Vtable(
      "ddraw.dll", "IDirectDrawSurface",
      {"QueryInterface", "AddRef", "Release", "AddAttachedSurface",
       "AddOverlayDirtyRect", "Blt", "BltBatch", "BltFast",
       "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders",
       "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper",
       "GetColorKey", "GetDC", "GetFlipStatus", "GetOverlayPosition",
       "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize",
       "IsLost", "Lock", "ReleaseDC", "Restore", "SetClipper", "SetColorKey",
       "SetOverlayPosition", "SetPalette", "Unlock", "UpdateOverlay",
       "UpdateOverlayDisplay", "UpdateOverlayZOrder"});
}

guest::DDSURFACEDESC ToDesc1(const DDSURFACEDESC2& d2) {
  guest::DDSURFACEDESC d1{};
  std::memcpy(&d1, &d2, offsetof(guest::DDSURFACEDESC, ddsCaps));
  d1.dwSize = sizeof d1;
  d1.ddsCaps.dwCaps = d2.ddsCaps.dwCaps;
  return d1;
}

DDSURFACEDESC2 ToDesc2(const guest::DDSURFACEDESC& d1) {
  DDSURFACEDESC2 d2{};
  std::memcpy(&d2, &d1, offsetof(guest::DDSURFACEDESC, ddsCaps));
  d2.dwSize = sizeof d2;
  d2.ddsCaps.dwCaps = d1.ddsCaps.dwCaps;
  return d2;
}

// A surface (and the chain attached to it) answers through the version 1
// methods: its guest pointer gets the version 1 vtable.
void UseVersion1(Surface& s) {
  Wr32(s.guest, Surface1Vtable());
  for (auto& a : s.attached) UseVersion1(*a);
}

// DirectDrawCreate(lpGUID, lplpDD, pUnkOuter): the primary display only.
HLE("ddraw.dll", DirectDrawCreate, 3) {
  const uint32_t guid = Arg(c, 0), out = Arg(c, 1);
  if (!out) return DDERR_INVALIDPARAMS;
  if (guid > 2 && Rd32(guid)) {  // DDCREATE_* values or a NULL GUID
    Wr32(out, 0);
    return DDERR_INVALIDPARAMS;
  }
  if (Arg(c, 2)) return 0x80040110;  // CLASS_E_NOAGGREGATION
  auto dd = std::make_shared<DirectDraw>();
  Wr32(out, com::Publish(dd, DirectDraw1Vtable()));
  return DD_OK;
}

// ---- IDirectDraw ------------------------------------------------------------------

// Only IDirectDraw itself: a game that asks for a later interface (to
// look for Direct3D) gets E_NOINTERFACE and keeps to DirectDraw 1.
HLE_NAMED("ddraw.dll", "IDirectDraw::QueryInterface", DD1_QueryInterface, 3) {
  const uint32_t self = Arg(c, 0), iid = Arg(c, 1), out = Arg(c, 2);
  if (com::GuidEq(iid, kIID_IDirectDraw)) {
    com::AddRef(self);
    Wr32(out, self);
    return DD_OK;
  }
  guest::GUID g = com::ReadGuid(iid);
  WarnF("IDirectDraw::QueryInterface({%08X-...}): no interface\n", g.Data1);
  Wr32(out, 0);
  return com::kE_NOINTERFACE;
}

DD1_ALIAS("IDirectDraw::AddRef", DD1_AddRef, 1, DD7_AddRef);
DD1_ALIAS("IDirectDraw::Release", DD1_Release, 1, DD7_Release);
DD1_ALIAS("IDirectDraw::Compact", DD1_Compact, 1, DD7_Compact);
DD1_ALIAS("IDirectDraw::FlipToGDISurface", DD1_FlipToGDISurface, 1, DD7_FlipToGDISurface);
DD1_ALIAS("IDirectDraw::GetCaps", DD1_GetCaps, 3, DD7_GetCaps);
DD1_ALIAS("IDirectDraw::GetFourCCCodes", DD1_GetFourCCCodes, 3, DD7_GetFourCCCodes);
DD1_ALIAS("IDirectDraw::GetMonitorFrequency", DD1_GetMonitorFrequency, 2, DD7_GetMonitorFrequency);
DD1_ALIAS("IDirectDraw::GetScanLine", DD1_GetScanLine, 2, DD7_GetScanLine);
DD1_ALIAS("IDirectDraw::GetVerticalBlankStatus", DD1_GetVerticalBlankStatus, 2,
          DD7_GetVerticalBlankStatus);
DD1_ALIAS("IDirectDraw::RestoreDisplayMode", DD1_RestoreDisplayMode, 1, DD7_RestoreDisplayMode);
DD1_ALIAS("IDirectDraw::SetCooperativeLevel", DD1_SetCooperativeLevel, 3, DD7_SetCooperativeLevel);
DD1_ALIAS("IDirectDraw::WaitForVerticalBlank", DD1_WaitForVerticalBlank, 3, DD7_WaitForVerticalBlank);

// (this, dwWidth, dwHeight, dwBPP): no refresh rate or flags in version 1.
HLE_NAMED("ddraw.dll", "IDirectDraw::SetDisplayMode", DD1_SetDisplayMode, 4) {
  SetMode(int(Arg(c, 1)), int(Arg(c, 2)), int(Arg(c, 3)));
  return DD_OK;
}

// (this, lpDDSurfaceDesc)
HLE_NAMED("ddraw.dll", "IDirectDraw::GetDisplayMode", DD1_GetDisplayMode, 2) {
  State& s = St();
  EnsureSdlVideo();
  DDSURFACEDESC2 d2;
  int w = s.mode_w ? s.mode_w : g_display_w, h = s.mode_h ? s.mode_h : g_display_h;
  FillModeDesc(&d2, w, h, s.mode_bpp ? s.mode_bpp : 32);
  *G<guest::DDSURFACEDESC>(Arg(c, 1)) = ToDesc1(d2);
  return DD_OK;
}

// (this, dwFlags, lpDDSurfaceDesc, lpContext, lpEnumModesCallback). A
// description narrows the modes to its width, height and bit depth (the
// fields its dwFlags give); the callback gets a DDSURFACEDESC and returns
// DDENUMRET_CANCEL (0) to stop.
HLE_NAMED("ddraw.dll", "IDirectDraw::EnumDisplayModes", DD1_EnumDisplayModes, 5) {
  const uint32_t want = Arg(c, 2), ctx = Arg(c, 3), cb = Arg(c, 4);
  EnsureSdlVideo();
  guest::DDSURFACEDESC filter{};
  if (want) filter = *G<guest::DDSURFACEDESC>(want);
  const uint32_t d = HeapAllocProcess(sizeof(guest::DDSURFACEDESC), true);
  DDSURFACEDESC2 d2;
  for (int bpp : {16, 32})
    for (const ModeSize& m : DisplayModeSizes()) {
      if ((filter.dwFlags & DDSD_WIDTH) && filter.dwWidth != uint32_t(m.w)) continue;
      if ((filter.dwFlags & DDSD_HEIGHT) && filter.dwHeight != uint32_t(m.h)) continue;
      if ((filter.dwFlags & DDSD_PIXELFORMAT) &&
          filter.ddpfPixelFormat.dwRGBBitCount != uint32_t(bpp))
        continue;
      FillModeDesc(&d2, m.w, m.h, bpp);
      *G<guest::DDSURFACEDESC>(d) = ToDesc1(d2);
      if (!CallGuest(c, cb, {d, ctx})) goto done;
    }
done:
  HeapFreeProcess(d);
  return DD_OK;
}

// (this, lpDDSurfaceDesc, lplpDDSurface, pUnkOuter)
HLE_NAMED("ddraw.dll", "IDirectDraw::CreateSurface", DD1_CreateSurface, 4) {
  if (Arg(c, 3)) return 0x80040110;  // CLASS_E_NOAGGREGATION
  uint32_t out = 0;
  const uint32_t hr = CreateSurfaceImpl(ToDesc2(*G<guest::DDSURFACEDESC>(Arg(c, 1))), &out);
  if (hr == DD_OK) UseVersion1(*com::As<Surface>(out));
  RECOMP_TRACE("  CreateSurface -> %X\n", out);
  Wr32(Arg(c, 2), out);
  return hr;
}

// ---- IDirectDrawClipper -------------------------------------------------------------
// A clipper only carries its window: blits to the primary are clipped to
// the surface, which is the window's client area here.

struct Clipper : com::Object {
  uint32_t hwnd = 0;
};

uint32_t ClipperVtable() {
  return com::Vtable("ddraw.dll", "IDirectDrawClipper",
                     {"QueryInterface", "AddRef", "Release", "GetClipList", "GetHWnd",
                      "Initialize", "IsClipListChanged", "SetClipList", "SetHWnd"});
}

// IDirectDraw::CreateClipper(this, dwFlags, lplpDDClipper, pUnkOuter)
HLE_NAMED("ddraw.dll", "IDirectDraw::CreateClipper", DD1_CreateClipper, 4) {
  if (Arg(c, 3)) return 0x80040110;
  Wr32(Arg(c, 2), com::Publish(std::make_shared<Clipper>(), ClipperVtable()));
  return DD_OK;
}

HLE_NAMED("ddraw.dll", "IDirectDrawClipper::AddRef", DDC_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::Release", DDC_Release, 1) { return com::Release(Arg(c, 0)); }
// (this, dwFlags, hWnd)
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::SetHWnd", DDC_SetHWnd, 3) {
  Clipper* k = com::As<Clipper>(Arg(c, 0));
  if (!k) return DDERR_INVALIDPARAMS;
  k->hwnd = Arg(c, 2);
  return DD_OK;
}
// (this, lphWnd)
HLE_NAMED("ddraw.dll", "IDirectDrawClipper::GetHWnd", DDC_GetHWnd, 2) {
  Clipper* k = com::As<Clipper>(Arg(c, 0));
  if (!k) return DDERR_INVALIDPARAMS;
  Wr32(Arg(c, 1), k->hwnd);
  return DD_OK;
}

// ---- IDirectDrawSurface ---------------------------------------------------------------

HLE_NAMED("ddraw.dll", "IDirectDrawSurface::QueryInterface", DDS1_QueryInterface, 3) {
  const uint32_t self = Arg(c, 0), iid = Arg(c, 1), out = Arg(c, 2);
  if (com::GuidEq(iid, kIID_IDirectDrawSurface)) {
    com::AddRef(self);
    Wr32(out, self);
    return DD_OK;
  }
  guest::GUID g = com::ReadGuid(iid);
  WarnF("IDirectDrawSurface::QueryInterface({%08X-...}): no interface\n", g.Data1);
  Wr32(out, 0);
  return com::kE_NOINTERFACE;
}

DD1_ALIAS("IDirectDrawSurface::AddRef", DDS1_AddRef, 1, DDS7_AddRef);
DD1_ALIAS("IDirectDrawSurface::Release", DDS1_Release, 1, DDS7_Release);
DD1_ALIAS("IDirectDrawSurface::AddAttachedSurface", DDS1_AddAttachedSurface, 2, DDS7_AddAttachedSurface);
DD1_ALIAS("IDirectDrawSurface::DeleteAttachedSurface", DDS1_DeleteAttachedSurface, 3,
          DDS7_DeleteAttachedSurface);
DD1_ALIAS("IDirectDrawSurface::Blt", DDS1_Blt, 6, DDS7_Blt);
DD1_ALIAS("IDirectDrawSurface::BltFast", DDS1_BltFast, 6, DDS7_BltFast);
DD1_ALIAS("IDirectDrawSurface::Flip", DDS1_Flip, 3, DDS7_Flip);
DD1_ALIAS("IDirectDrawSurface::GetBltStatus", DDS1_GetBltStatus, 2, DDS7_GetBltStatus);
DD1_ALIAS("IDirectDrawSurface::GetFlipStatus", DDS1_GetFlipStatus, 2, DDS7_GetFlipStatus);
DD1_ALIAS("IDirectDrawSurface::GetColorKey", DDS1_GetColorKey, 3, DDS7_GetColorKey);
DD1_ALIAS("IDirectDrawSurface::SetColorKey", DDS1_SetColorKey, 3, DDS7_SetColorKey);
DD1_ALIAS("IDirectDrawSurface::GetPixelFormat", DDS1_GetPixelFormat, 2, DDS7_GetPixelFormat);
DD1_ALIAS("IDirectDrawSurface::IsLost", DDS1_IsLost, 1, DDS7_IsLost);
DD1_ALIAS("IDirectDrawSurface::Restore", DDS1_Restore, 1, DDS7_Restore);
DD1_ALIAS("IDirectDrawSurface::SetClipper", DDS1_SetClipper, 2, DDS7_SetClipper);
// Version 1's Unlock takes the pointer Lock returned instead of a RECT;
// neither is needed to find what was locked.
DD1_ALIAS("IDirectDrawSurface::Unlock", DDS1_Unlock, 2, DDS7_Unlock);

// (this, lpDDSurfaceDesc)
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetSurfaceDesc", DDS1_GetSurfaceDesc, 2) {
  DDSURFACEDESC2 d = Self(c)->desc;
  d.lpSurface = 0;
  *G<guest::DDSURFACEDESC>(Arg(c, 1)) = ToDesc1(d);
  return DD_OK;
}

// (this, lpDDSCaps): one DWORD.
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetCaps", DDS1_GetCaps, 2) {
  Wr32(Arg(c, 1), Self(c)->desc.ddsCaps.dwCaps);
  return DD_OK;
}

// (this, lpDDSCaps, lplpDDAttachedSurface): lpDDSCaps is one DWORD.
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::GetAttachedSurface", DDS1_GetAttachedSurface, 3) {
  Surface* s = Self(c);
  const uint32_t want = Rd32(Arg(c, 1));
  for (auto& a : s->attached) {
    if ((a->desc.ddsCaps.dwCaps & want) == want) {
      com::AddRef(a->guest);
      Wr32(Arg(c, 2), a->guest);
      return DD_OK;
    }
  }
  Wr32(Arg(c, 2), 0);
  return DDERR_NOTFOUND;
}

// (this, lpDestRect, lpDDSurfaceDesc, dwFlags, hEvent)
HLE_NAMED("ddraw.dll", "IDirectDrawSurface::Lock", DDS1_Lock, 5) {
  DDSURFACEDESC2 d;
  const uint32_t hr = LockImpl(Self(c), Arg(c, 1), Arg(c, 3), &d);
  if (hr == DD_OK) *G<guest::DDSURFACEDESC>(Arg(c, 2)) = ToDesc1(d);
  return hr;
}

}  // namespace
}  // namespace recomp::hle
