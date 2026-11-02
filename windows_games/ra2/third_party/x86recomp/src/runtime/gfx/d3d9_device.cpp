// gfx::Device on Direct3D 9 through dxvk-native (Linux bring-up backend).
//
// This file is the only one that sees dxvk's Windows/D3D9 headers. D3D7
// semantics map almost 1:1 onto D3D9's fixed-function pipeline; the
// differences handled here:
//   - texture addressing/filtering moved from texture stage states to
//     sampler states, with renumbered filter enums;
//   - WORLD transforms are 256+ in D3D9;
//   - D3D7's integer ZBIAS becomes D3D9's float DEPTHBIAS;
//   - render states D3D9 dropped are ignored (logged once).

#include <windows.h>
#include <d3d9.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/log.h"

namespace recomp::gfx {

namespace {

D3DFORMAT ToD3D(Format f) {
  switch (f) {
    case Format::X8R8G8B8: return D3DFMT_X8R8G8B8;
    case Format::A8R8G8B8: return D3DFMT_A8R8G8B8;
    case Format::R8G8B8: return D3DFMT_R8G8B8;
    case Format::R5G6B5: return D3DFMT_R5G6B5;
    case Format::X1R5G5B5: return D3DFMT_X1R5G5B5;
    case Format::A1R5G5B5: return D3DFMT_A1R5G5B5;
    case Format::A4R4G4B4: return D3DFMT_A4R4G4B4;
    case Format::X4R4G4B4: return D3DFMT_X4R4G4B4;
    case Format::R3G3B2: return D3DFMT_R3G3B2;
    case Format::A8: return D3DFMT_A8;
    case Format::L8: return D3DFMT_L8;
    case Format::A8L8: return D3DFMT_A8L8;
    case Format::P8: return D3DFMT_P8;
    case Format::D16: return D3DFMT_D16;
    case Format::D24X8: return D3DFMT_D24X8;
    case Format::D24S8: return D3DFMT_D24S8;
    case Format::D32: return D3DFMT_D32;
    case Format::D15S1: return D3DFMT_D15S1;
    case Format::DXT1: return D3DFMT_DXT1;
    case Format::DXT2: return D3DFMT_DXT2;
    case Format::DXT3: return D3DFMT_DXT3;
    case Format::DXT4: return D3DFMT_DXT4;
    case Format::DXT5: return D3DFMT_DXT5;
    case Format::R16F: return D3DFMT_R16F;
    case Format::G16R16F: return D3DFMT_G16R16F;
    case Format::A16B16G16R16F: return D3DFMT_A16B16G16R16F;
    case Format::R32F: return D3DFMT_R32F;
    case Format::G32R32F: return D3DFMT_G32R32F;
    case Format::A32B32G32R32F: return D3DFMT_A32B32G32R32F;
    default: return D3DFMT_UNKNOWN;
  }
}

void Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) FatalF("d3d9: %s failed (%08lx)", what, (unsigned long)hr);
}

class D3D9Device final : public Device {
 public:
  explicit D3D9Device(SDL_Window* w) : window_(w) {
    // dxvk-native refuses to start without a WSI backend name.
    setenv("DXVK_WSI_DRIVER", "SDL3", 0);
    // Without vsync, present in mailbox mode (no tearing, never blocks)
    // rather than immediate. A tearFree setting the user made wins.
    const char* conf = std::getenv("DXVK_CONFIG");
    if (!conf || !std::strstr(conf, "tearFree")) {
      std::string v = conf && *conf ? std::string(conf) + ";" : std::string();
      v += "dxvk.tearFree = True";
      setenv("DXVK_CONFIG", v.c_str(), 1);
    }
    d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d_) FatalF("Direct3DCreate9 failed");
  }

  ~D3D9Device() override {
    for (auto& [id, t] : textures_) t->Release();
    for (auto& [id, t] : cubes_) t->Release();
    for (auto& [id, d] : depths_) d.surf->Release();
    if (staging_) staging_->Release();
    if (readback_) readback_->Release();
    ReleaseRings();
    ReleaseTargets();
    if (dev_) dev_->Release();
    if (d3d_) d3d_->Release();
  }

  // Always X8R8G8B8; 16-bit modes convert on blit. The swap chain has
  // the window's size; PresentFront fits the w x h front buffer into it.
  void ResetSwapChain(int w, int h, bool vsync) override {
    bb_w_ = w;
    bb_h_ = h;
    vsync_ = vsync;
    WindowPixels(&swap_w_, &swap_h_);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = UINT(swap_w_);
    pp.BackBufferHeight = UINT(swap_h_);
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = reinterpret_cast<HWND>(window_);
    pp.PresentationInterval =
        vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    ReleaseTargets();
    bool reset = false;
    if (!dev_) {
      Check(d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                               reinterpret_cast<HWND>(window_),
                               D3DCREATE_HARDWARE_VERTEXPROCESSING |
                                   D3DCREATE_FPU_PRESERVE |
                                   D3DCREATE_MULTITHREADED,
                               &pp, &dev_),
            "CreateDevice");
      D3DADAPTER_IDENTIFIER9 id{};
      d3d_->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
      std::fprintf(stderr, "gfx: d3d9 (dxvk-native) on %s, %dx%d\n",
                   id.Description, w, h);
    } else {
      // Default-pool resources must go before Reset. Render-target
      // textures come back empty; the game redraws them every frame.
      for (auto& [id, d] : depths_) d.surf->Release();
      for (auto& [id, r] : targets_) textures_[id]->Release();
      if (staging_) {
        staging_->Release();
        staging_ = nullptr;
      }
      if (readback_) {
        readback_->Release();
        readback_ = nullptr;
      }
      ReleaseRings();
      Check(dev_->Reset(&pp), "Reset");
      reset = true;
      for (auto& [id, d] : depths_)
        Check(dev_->CreateDepthStencilSurface(UINT(d.w), UINT(d.h), ToD3D(d.fmt),
                                              D3DMULTISAMPLE_NONE, 0, FALSE,
                                              &d.surf, nullptr),
              "CreateDepthStencilSurface");
      for (auto& [id, r] : targets_)
        Check(dev_->CreateTexture(UINT(r.w), UINT(r.h), UINT(r.levels), D3DUSAGE_RENDERTARGET,
                                  ToD3D(r.fmt), D3DPOOL_DEFAULT, &textures_[id], nullptr),
              "CreateTexture(render target)");
    }
    Check(dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &swap_),
          "GetBackBuffer");
    // The game draws into its own back buffer; Present copies that to the
    // front buffer, which is then shown. Keeping the front separate lets
    // the game read and draw on it between frames without touching the
    // next frame (as with a DirectDraw primary surface).
    for (IDirect3DSurface9** t : {&backbuffer_, &front_}) {
      Check(dev_->CreateRenderTarget(UINT(w), UINT(h), D3DFMT_X8R8G8B8,
                                     D3DMULTISAMPLE_NONE, 0, FALSE, t, nullptr),
            "CreateRenderTarget");
      dev_->ColorFill(*t, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    }
    dev_->SetRenderTarget(0, backbuffer_);
    if (reset) {
      // Reset also unbinds the targets.
      SetRenderTarget(current_color_, current_depth_);
      RestoreState();
    }
  }

  TextureId CreateTexture(int w, int h, int levels, Format fmt,
                          bool rt) override {
    IDirect3DTexture9* t = nullptr;
    HRESULT hr = dev_->CreateTexture(UINT(w), UINT(h), UINT(levels),
                                     rt ? D3DUSAGE_RENDERTARGET : 0,
                                     ToD3D(fmt),
                                     rt ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
                                     &t, nullptr);
    if (FAILED(hr)) {
      WarnF("gfx: CreateTexture %dx%d x%d fmt %u failed (%08lx)\n", w, h,
            levels, unsigned(fmt), (unsigned long)hr);
      return 0;
    }
    TextureId id = next_id_++;
    textures_[id] = t;
    if (rt) targets_[id] = {w, h, levels, fmt};
    return id;
  }

  void DestroyTexture(TextureId id) override {
    auto cube = cubes_.find(id);
    if (cube != cubes_.end()) {
      cube->second->Release();
      cubes_.erase(cube);
      return;
    }
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    it->second->Release();
    textures_.erase(it);
    targets_.erase(id);
  }

  TextureId CreateCubeTexture(int size, int levels, Format fmt) override {
    IDirect3DCubeTexture9* t = nullptr;
    HRESULT hr = dev_->CreateCubeTexture(UINT(size), UINT(levels), 0, ToD3D(fmt),
                                         D3DPOOL_MANAGED, &t, nullptr);
    if (FAILED(hr)) {
      WarnF("gfx: CreateCubeTexture %d x%d fmt %u failed (%08lx)\n", size, levels,
            unsigned(fmt), (unsigned long)hr);
      return 0;
    }
    TextureId id = next_id_++;
    cubes_[id] = t;
    return id;
  }

  void UpdateCubeTexture(TextureId id, int face, int level, const Rect& r, const void* src,
                         uint32_t pitch) override {
    auto it = cubes_.find(id);
    if (it == cubes_.end()) return;
    D3DSURFACE_DESC desc;
    it->second->GetLevelDesc(UINT(level), &desc);
    Format f = FromD3D(desc.Format);
    RECT rc{r.left, r.top, r.right, r.bottom};
    D3DLOCKED_RECT lr;
    if (FAILED(it->second->LockRect(D3DCUBEMAP_FACES(face), UINT(level), &lr, &rc, 0))) return;
    uint32_t row = RowPitch(f, uint32_t(r.right - r.left));
    uint32_t rows = RowCount(f, uint32_t(r.bottom - r.top));
    const uint8_t* s = static_cast<const uint8_t*>(src);
    uint8_t* d = static_cast<uint8_t*>(lr.pBits);
    for (uint32_t y = 0; y < rows; y++)
      std::memcpy(d + size_t(y) * uint32_t(lr.Pitch), s + size_t(y) * pitch, row);
    it->second->UnlockRect(D3DCUBEMAP_FACES(face), UINT(level));
  }

  void FillTexture(TextureId id, const Rect& r, uint32_t argb) override {
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    IDirect3DSurface9* s = nullptr;
    it->second->GetSurfaceLevel(0, &s);
    RECT rc{r.left, r.top, r.right, r.bottom};
    dev_->ColorFill(s, &rc, argb);
    s->Release();
  }

  void UpdateTexture(TextureId id, int level, const Rect& r, const void* src,
                     uint32_t pitch) override {
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    D3DSURFACE_DESC desc;
    it->second->GetLevelDesc(UINT(level), &desc);
    Format f = FromD3D(desc.Format);
    RECT rc{r.left, r.top, r.right, r.bottom};
    D3DLOCKED_RECT lr;
    if (FAILED(it->second->LockRect(UINT(level), &lr, &rc, 0))) return;
    uint32_t row = RowPitch(f, uint32_t(r.right - r.left));
    uint32_t rows = RowCount(f, uint32_t(r.bottom - r.top));
    const uint8_t* s = static_cast<const uint8_t*>(src);
    uint8_t* d = static_cast<uint8_t*>(lr.pBits);
    for (uint32_t y = 0; y < rows; y++)
      std::memcpy(d + size_t(y) * uint32_t(lr.Pitch), s + size_t(y) * pitch, row);
    it->second->UnlockRect(UINT(level));
  }

  DepthId CreateDepth(int w, int h, Format fmt) override {
    Depth d{nullptr, w, h, fmt};
    if (FAILED(dev_->CreateDepthStencilSurface(UINT(w), UINT(h), ToD3D(fmt),
                                               D3DMULTISAMPLE_NONE, 0, FALSE,
                                               &d.surf, nullptr))) {
      WarnF("gfx: CreateDepth %dx%d fmt %u failed\n", w, h, unsigned(fmt));
      return 0;
    }
    DepthId id = next_id_++;
    depths_[id] = d;
    return id;
  }

  void DestroyDepth(DepthId id) override {
    auto it = depths_.find(id);
    if (it == depths_.end()) return;
    if (current_depth_ == id) {
      dev_->SetDepthStencilSurface(nullptr);
      current_depth_ = 0;
    }
    it->second.surf->Release();
    depths_.erase(it);
  }

  // The programmable pipeline runs GLSL translated from HLSL, which this
  // backend cannot: it stays the reference for fixed-function games.
  void SetRenderTargets(const ColorTarget color[4], DepthId depth) override {
    if (color[1].tex) FatalF("gfx: multiple render targets need the SDL_GPU backend (<ID>_GFX=gpu)");
    if (color[0].level) FatalF("gfx: rendering to a mip level needs the SDL_GPU backend (<ID>_GFX=gpu)");
    SetRenderTarget(color[0].tex, depth);
  }
  void StretchRect(ColorTarget, const Rect&, ColorTarget, const Rect&, bool) override {
    FatalF("gfx: StretchRect needs the SDL_GPU backend (<ID>_GFX=gpu)");
  }
  ShaderId CreateShader(const hlsl::Program&) override {
    FatalF("gfx: translated shaders need the SDL_GPU backend (<ID>_GFX=gpu)");
  }
  void DestroyShader(ShaderId) override {}
  void DrawShader(const ShaderDraw&) override {
    FatalF("gfx: translated shaders need the SDL_GPU backend (<ID>_GFX=gpu)");
  }

  void SetRenderTarget(TextureId color, DepthId depth) override {
    current_color_ = color;
    if (color) {
      auto it = textures_.find(color);
      if (it != textures_.end()) {
        IDirect3DSurface9* s = nullptr;
        it->second->GetSurfaceLevel(0, &s);
        dev_->SetRenderTarget(0, s);
        s->Release();
      }
    } else {
      dev_->SetRenderTarget(0, backbuffer_);
    }
    auto d = depths_.find(depth);
    dev_->SetDepthStencilSurface(d == depths_.end() ? nullptr : d->second.surf);
    current_depth_ = depth;
  }

  void Blit(const void* src, uint32_t pitch, Format fmt, const Rect& sr,
            const Rect& dr, bool colorkey, uint32_t key) override {
    DrawImage(src, pitch, fmt, sr, dr, colorkey, key);
  }

  void BlitFront(const void* src, uint32_t pitch, Format fmt, const Rect& sr,
                 const Rect& dr) override {
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    D3DVIEWPORT9 vp;
    dev_->GetRenderTarget(0, &rt);
    dev_->GetDepthStencilSurface(&ds);
    dev_->GetViewport(&vp);
    dev_->SetRenderTarget(0, front_);
    dev_->SetDepthStencilSurface(nullptr);
    DrawImage(src, pitch, fmt, sr, dr, false, 0);
    dev_->SetRenderTarget(0, rt);
    dev_->SetDepthStencilSurface(ds);
    dev_->SetViewport(&vp);
    if (rt) rt->Release();
    if (ds) ds->Release();
  }

  void ReadBackFront(const Rect& r, void* dst, uint32_t pitch, Format fmt) override {
    ReadSurface(front_, r, dst, pitch, fmt);
  }

  // Front buffer to the window: scaled to fit with its aspect kept, black
  // around it (FitRect). When the window has changed size the swap chain
  // follows it, which costs a device Reset: the front and back buffers
  // start black again and that frame is lost.
  void PresentFront() override {
    if (in_scene_) EndScene();
    int ww, wh;
    WindowPixels(&ww, &wh);
    if (ww != swap_w_ || wh != swap_h_) ResetSwapChain(bb_w_, bb_h_, vsync_);
    const Rect r = FitRect(bb_w_, bb_h_, swap_w_, swap_h_);
    const RECT dst{r.left, r.top, r.right, r.bottom};
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w != swap_w_ || h != swap_h_) dev_->ColorFill(swap_, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    const bool same = w == bb_w_ && h == bb_h_;
    dev_->StretchRect(front_, nullptr, swap_, &dst, same ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    dev_->Present(nullptr, nullptr, nullptr, nullptr);
    NotePresentation({bb_w_, bb_h_, swap_w_, swap_h_, r});
  }

  void BlitBlend(const void* src, uint32_t pitch, const Rect& sr, const Rect& dr) override {
    DrawImage(src, pitch, Format::A8R8G8B8, sr, dr, false, 0, true);
  }

  void ReadBack(const Rect& r, void* dst, uint32_t pitch, Format fmt) override {
    IDirect3DSurface9* rt = nullptr;
    dev_->GetRenderTarget(0, &rt);
    ReadSurface(rt, r, dst, pitch, fmt);
    rt->Release();
  }

  // Reads at once (GetRenderTargetData waits anyway) and keeps the
  // pixels for FinishReadBack.
  uint32_t StartReadBack(const Rect& r) override {
    uint32_t t = next_read_++;
    if (!t) t = next_read_++;
    Read& rd = reads_[t];
    rd.r = r;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w > 0 && h > 0) {
      rd.px.resize(size_t(w) * h);
      ReadBack(r, rd.px.data(), uint32_t(w) * 4, Format::A8R8G8B8);
    }
    return t;
  }

  void FinishReadBack(uint32_t ticket, void* dst, uint32_t pitch, Format fmt) override {
    auto it = reads_.find(ticket);
    if (it == reads_.end()) return;
    const Read& rd = it->second;
    int w = rd.r.right - rd.r.left, h = rd.r.bottom - rd.r.top;
    if (dst && !rd.px.empty()) {
      uint32_t bpp = BitsPerPixel(fmt) / 8;
      for (int y = 0; y < h; y++) {
        const uint32_t* s = rd.px.data() + size_t(y) * w;
        uint8_t* out = static_cast<uint8_t*>(dst) + size_t(y) * pitch;
        if (fmt == Format::X8R8G8B8 || fmt == Format::A8R8G8B8)
          std::memcpy(out, s, size_t(w) * 4);
        else
          for (int x = 0; x < w; x++) EncodePixel(out + size_t(x) * bpp, fmt, s[x]);
      }
    }
    reads_.erase(it);
  }

  void SetRenderState(uint32_t s, uint32_t v) override {
    state_.rs[s] = v;
    switch (s) {
      case 47: {  // D3DRENDERSTATE_ZBIAS (0..16) -> DEPTHBIAS
        float bias = -float(v) * 0.000005f;
        uint32_t bits;
        std::memcpy(&bits, &bias, 4);
        dev_->SetRenderState(D3DRS_DEPTHBIAS, bits);
        return;
      }
      // D3D7 states that D3D9 dropped or that have no effect here.
      case 1: case 2: case 3: case 4: case 5: case 6: case 10: case 11:
      case 12: case 13: case 17: case 18: case 21: case 30: case 31:
      case 32: case 33: case 39: case 40: case 41: case 42: case 43:
      case 44: case 45: case 46: case 49: case 50: case 138: case 144:
        if (ignored_rs_.insert(s).second)
          RECOMP_TRACE("gfx: render state %u = %u ignored\n", s, v);
        return;
      default:
        dev_->SetRenderState(D3DRENDERSTATETYPE(s), v);
    }
  }

  void SetTextureStageState(uint32_t stage, uint32_t type, uint32_t v) override {
    state_.tss[{stage, type}] = v;
    switch (type) {
      case 12:  // ADDRESS (both)
        dev_->SetSamplerState(stage, D3DSAMP_ADDRESSU, v);
        dev_->SetSamplerState(stage, D3DSAMP_ADDRESSV, v);
        return;
      case 13: dev_->SetSamplerState(stage, D3DSAMP_ADDRESSU, v); return;
      case 14: dev_->SetSamplerState(stage, D3DSAMP_ADDRESSV, v); return;
      case 15: dev_->SetSamplerState(stage, D3DSAMP_BORDERCOLOR, v); return;
      case 16: {  // MAGFILTER: D3DTFG_POINT 1, LINEAR 2, cubic 3/4, ANISO 5
        DWORD f = v == 1 ? D3DTEXF_POINT : v == 5 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR;
        dev_->SetSamplerState(stage, D3DSAMP_MAGFILTER, f);
        return;
      }
      case 17: {  // MINFILTER: D3DTFN_POINT 1, LINEAR 2, ANISOTROPIC 3
        DWORD f = v == 1 ? D3DTEXF_POINT : v == 3 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR;
        dev_->SetSamplerState(stage, D3DSAMP_MINFILTER, f);
        return;
      }
      case 18: {  // MIPFILTER: D3DTFP_NONE 1, POINT 2, LINEAR 3
        DWORD f = v == 2 ? D3DTEXF_POINT : v == 3 ? D3DTEXF_LINEAR : D3DTEXF_NONE;
        dev_->SetSamplerState(stage, D3DSAMP_MIPFILTER, f);
        return;
      }
      case 19: dev_->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, v); return;
      case 20: dev_->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, v); return;
      case 21: dev_->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, v); return;
      default:
        dev_->SetTextureStageState(stage, D3DTEXTURESTAGESTATETYPE(type), v);
    }
  }

  void SetTexture(uint32_t stage, TextureId id) override {
    if (stage >= 8) return;
    state_.tex[stage] = id;
    auto it = textures_.find(id);
    dev_->SetTexture(stage, it == textures_.end() ? nullptr : it->second);
  }

  void SetTransform(uint32_t type, const float m[16]) override {
    std::memcpy(state_.xf[type].data(), m, sizeof(float) * 16);
    D3DTRANSFORMSTATETYPE t;
    switch (type) {
      case 1: t = D3DTS_WORLD; break;
      case 4: t = D3DTS_WORLD1; break;
      case 5: t = D3DTS_WORLD2; break;
      case 6: t = D3DTS_WORLD3; break;
      default: t = D3DTRANSFORMSTATETYPE(type); break;
    }
    dev_->SetTransform(t, reinterpret_cast<const D3DMATRIX*>(m));
  }

  void SetViewport(const Viewport& v) override {
    state_.vp = v;
    D3DVIEWPORT9 vp{v.x, v.y, v.width, v.height, v.min_z, v.max_z};
    dev_->SetViewport(&vp);
  }

  void SetScissor(const Rect& r) override {
    state_.scissor = r;
    RECT rc{r.left, r.top, r.right, r.bottom};
    dev_->SetScissorRect(&rc);
  }

  void SetMaterial(const Material& m) override {
    state_.mat = m;
    static_assert(sizeof(Material) == sizeof(D3DMATERIAL9));
    dev_->SetMaterial(reinterpret_cast<const D3DMATERIAL9*>(&m));
  }

  void SetLight(uint32_t i, const Light& l) override {
    state_.lights[i] = l;
    static_assert(sizeof(Light) == sizeof(D3DLIGHT9));
    dev_->SetLight(i, reinterpret_cast<const D3DLIGHT9*>(&l));
  }

  void LightEnable(uint32_t i, bool e) override {
    state_.light_on[i] = e;
    dev_->LightEnable(i, e);
  }

  void SetClipPlane(uint32_t i, const float p[4]) override {
    std::memcpy(state_.clip[i].data(), p, sizeof(float) * 4);
    dev_->SetClipPlane(i, p);
  }

  void Clear(uint32_t count, const Rect* rects, uint32_t flags, uint32_t color,
             float z, uint32_t stencil) override {
    static_assert(sizeof(Rect) == sizeof(D3DRECT));
    if (!current_depth_) flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    dev_->Clear(count, reinterpret_cast<const D3DRECT*>(rects), flags, color, z,
                stencil);
  }

  void BeginScene() override {
    if (!in_scene_) dev_->BeginScene();
    in_scene_ = true;
  }
  void EndScene() override {
    if (in_scene_) dev_->EndScene();
    in_scene_ = false;
  }

  void Draw(uint32_t prim, uint32_t fvf, const void* v, uint32_t nv,
            uint32_t stride, const uint16_t* idx, uint32_t ni) override {
    dev_->SetFVF(fvf);
    if (idx) {
      UINT prims = PrimitiveCountFromVertices(prim, ni);
      if (prims)
        dev_->DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE(prim), 0, nv, prims, idx,
                                     D3DFMT_INDEX16, v, stride);
    } else {
      UINT prims = PrimitiveCountFromVertices(prim, nv);
      if (prims) dev_->DrawPrimitiveUP(D3DPRIMITIVETYPE(prim), prims, v, stride);
    }
  }

  Mapped MapDraw(uint32_t nv, uint32_t stride, uint32_t ni) override {
    mapped_ = {nv, stride, ni};
    // Vertices start at a multiple of the stride: the draw finds them by
    // base vertex index.
    auto* v = static_cast<uint8_t*>(LockRing(vb_, stride, nv * stride, &mapped_.vofs));
    auto* i = static_cast<uint16_t*>(LockRing(ib_, 2, ni * 2, &mapped_.iofs));
    return {v, i};
  }

  void DrawMapped(uint32_t prim, uint32_t fvf) override {
    vb_.buf->Unlock();
    ib_.buf->Unlock();
    const MappedDraw& m = mapped_;
    dev_->SetFVF(fvf);
    // Set every time: the UP draws (DrawImage) unbind both.
    dev_->SetStreamSource(0, vb_.buf, 0, m.stride);
    dev_->SetIndices(ib_.buf);
    UINT prims = PrimitiveCountFromVertices(prim, m.ni);
    if (prims)
      dev_->DrawIndexedPrimitive(D3DPRIMITIVETYPE(prim), INT(m.vofs / m.stride), 0, m.nv,
                                 m.iofs / 2, prims);
  }

  void Present() override {
    if (in_scene_) EndScene();
    dev_->StretchRect(backbuffer_, nullptr, front_, nullptr, D3DTEXF_POINT);
    PresentFront();
  }

 private:
  // A blit; with `blend`, an A8R8G8B8 image blended by its alpha.
  void DrawImage(const void* src, uint32_t pitch, Format fmt, const Rect& sr,
                 const Rect& dr, bool colorkey, uint32_t key, bool blend = false) {
    int w = sr.right - sr.left, h = sr.bottom - sr.top;
    if (w <= 0 || h <= 0) return;
    EnsureStaging(w, h);
    D3DLOCKED_RECT lr;
    RECT rc{0, 0, w, h};
    if (FAILED(staging_->LockRect(0, &lr, &rc, 0))) return;
    uint32_t bpp = BitsPerPixel(fmt) / 8;
    for (int y = 0; y < h; y++) {
      const uint8_t* s = static_cast<const uint8_t*>(src) +
                         size_t(sr.top + y) * pitch + size_t(sr.left) * bpp;
      uint32_t* d = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) +
                                                size_t(y) * uint32_t(lr.Pitch));
      if (!colorkey && (fmt == Format::X8R8G8B8 || fmt == Format::A8R8G8B8)) {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(s);
        const uint32_t opaque = blend ? 0 : 0xFF000000u;
        for (int x = 0; x < w; x++) d[x] = p[x] | opaque;
        continue;
      }
      for (int x = 0; x < w; x++, s += bpp) {
        if (colorkey && RawPixel(s, fmt) == key)
          d[x] = 0;
        else
          d[x] = DecodePixel(s, fmt) | 0xFF000000u;
      }
    }
    staging_->UnlockRect(0);

    // Draw it as a screen-space quad with private state.
    IDirect3DStateBlock9* sb = nullptr;
    dev_->CreateStateBlock(D3DSBT_ALL, &sb);
    dev_->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev_->SetTexture(0, staging_);
    for (DWORD i = 1; i < 8; i++) dev_->SetTexture(i, nullptr);
    dev_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev_->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev_->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev_->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev_->SetRenderState(D3DRS_ALPHABLENDENABLE, blend ? TRUE : FALSE);
    dev_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev_->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev_->SetRenderState(D3DRS_ALPHATESTENABLE, colorkey ? TRUE : FALSE);
    dev_->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    dev_->SetRenderState(D3DRS_ALPHAREF, 0);
    dev_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev_->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    D3DSURFACE_DESC td;
    staging_->GetLevelDesc(0, &td);
    float u1 = float(w) / float(td.Width), v1 = float(h) / float(td.Height);
    struct V {
      float x, y, z, rhw, u, v;
    } q[4] = {
        {float(dr.left) - 0.5f, float(dr.top) - 0.5f, 0, 1, 0, 0},
        {float(dr.right) - 0.5f, float(dr.top) - 0.5f, 0, 1, u1, 0},
        {float(dr.left) - 0.5f, float(dr.bottom) - 0.5f, 0, 1, 0, v1},
        {float(dr.right) - 0.5f, float(dr.bottom) - 0.5f, 0, 1, u1, v1},
    };
    bool in_scene = in_scene_;
    if (!in_scene) dev_->BeginScene();
    dev_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    if (!in_scene) dev_->EndScene();
    if (sb) {
      sb->Apply();
      sb->Release();
    }
  }

  void ReadSurface(IDirect3DSurface9* rt, const Rect& r, void* dst, uint32_t pitch,
                   Format fmt) {
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    if (readback_) {
      D3DSURFACE_DESC rd;
      readback_->GetDesc(&rd);
      if (rd.Width != d.Width || rd.Height != d.Height || rd.Format != d.Format) {
        readback_->Release();
        readback_ = nullptr;
      }
    }
    if (!readback_ &&
        FAILED(dev_->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format,
                                                 D3DPOOL_SYSTEMMEM, &readback_,
                                                 nullptr))) {
      readback_ = nullptr;
      return;
    }
    dev_->GetRenderTargetData(rt, readback_);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(readback_->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
      uint32_t bpp = BitsPerPixel(fmt) / 8;
      int w = r.right - r.left;
      for (int y = r.top; y < r.bottom; y++) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(lr.pBits) + size_t(y) * uint32_t(lr.Pitch)) + r.left;
        uint8_t* out = static_cast<uint8_t*>(dst) + size_t(y - r.top) * pitch;
        if (fmt == Format::X8R8G8B8 || fmt == Format::A8R8G8B8) {
          // The render target is X8R8G8B8 too: copy, opaque alpha.
          uint32_t* o = reinterpret_cast<uint32_t*>(out);
          for (int x = 0; x < w; x++) o[x] = src[x] | 0xFF000000u;
        } else if (fmt == Format::R5G6B5) {
          uint16_t* o = reinterpret_cast<uint16_t*>(out);
          for (int x = 0; x < w; x++) {
            uint32_t c = src[x];
            o[x] = uint16_t((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F));
          }
        } else {
          for (int x = 0; x < w; x++)
            EncodePixel(out + size_t(x) * bpp, fmt, src[x] | 0xFF000000u);
        }
      }
      readback_->UnlockRect();
    }
  }

  struct Depth {
    IDirect3DSurface9* surf;
    int w, h;
    Format fmt;
  };
  struct Target {
    int w, h, levels;
    Format fmt;
  };

  static Format FromD3D(D3DFORMAT f) {
    for (uint32_t i = 0; i <= uint32_t(Format::A32B32G32R32F); i++)
      if (ToD3D(Format(i)) == f) return Format(i);
    return Format::Unknown;
  }

  void WindowPixels(int* w, int* h) {
    *w = bb_w_;
    *h = bb_h_;
    int pw = 0, ph = 0;
    if (SDL_GetWindowSizeInPixels(window_, &pw, &ph) && pw > 0 && ph > 0) {
      *w = pw;
      *h = ph;
    }
  }

  void ReleaseTargets() {
    for (IDirect3DSurface9** t : {&swap_, &backbuffer_, &front_})
      if (*t) {
        (*t)->Release();
        *t = nullptr;
      }
  }

  // A dynamic buffer that draws take consecutive ranges of (NOOVERWRITE:
  // earlier draws may still read what came before), starting over in
  // fresh memory (DISCARD) when it is full. Not WRITEONLY: dxvk then keeps
  // it in cached host memory, which the CPU writes quickly, instead of
  // write-combined device memory.
  template <class B>
  struct Ring {
    B* buf = nullptr;
    uint32_t size = 0, pos = 0;
  };

  void Create(uint32_t size, IDirect3DVertexBuffer9** b) {
    Check(dev_->CreateVertexBuffer(size, D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, b, nullptr),
          "CreateVertexBuffer(ring)");
  }
  void Create(uint32_t size, IDirect3DIndexBuffer9** b) {
    Check(dev_->CreateIndexBuffer(size, D3DUSAGE_DYNAMIC, D3DFMT_INDEX16, D3DPOOL_DEFAULT, b,
                                  nullptr),
          "CreateIndexBuffer(ring)");
  }

  template <class B>
  void* LockRing(Ring<B>& r, uint32_t align, uint32_t bytes, uint32_t* offset) {
    uint32_t ofs = (r.pos + align - 1) / align * align;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (!r.buf || ofs + bytes > r.size) {
      if (bytes > r.size) {
        if (r.buf) r.buf->Release();
        r.size = std::max(r.size, 1u << 22);
        while (r.size < bytes) r.size *= 2;
        Create(r.size, &r.buf);
      }
      ofs = 0;
      flags = D3DLOCK_DISCARD;
    }
    void* p = nullptr;
    Check(r.buf->Lock(ofs, bytes, &p, flags), "Lock(ring)");
    r.pos = ofs + bytes;
    *offset = ofs;
    return p;
  }

  void ReleaseRings() {
    if (vb_.buf) vb_.buf->Release();
    if (ib_.buf) ib_.buf->Release();
    vb_ = {};
    ib_ = {};
  }

  void EnsureStaging(int w, int h) {
    if (staging_) {
      D3DSURFACE_DESC d;
      staging_->GetLevelDesc(0, &d);
      if (int(d.Width) >= w && int(d.Height) >= h) return;
      staging_->Release();
      staging_ = nullptr;
    }
    int sw = 256, sh = 256;
    while (sw < w) sw *= 2;
    while (sh < h) sh *= 2;
    Check(dev_->CreateTexture(UINT(sw), UINT(sh), 1, D3DUSAGE_DYNAMIC,
                              D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &staging_,
                              nullptr),
          "CreateTexture(staging)");
  }

  SDL_Window* window_;
  IDirect3D9* d3d_ = nullptr;
  IDirect3DDevice9* dev_ = nullptr;
  IDirect3DSurface9* swap_ = nullptr;        // the swap chain's back buffer
  IDirect3DSurface9* backbuffer_ = nullptr;  // the game's back buffer
  IDirect3DSurface9* front_ = nullptr;       // the DirectDraw primary
  int bb_w_ = 0, bb_h_ = 0;      // back and front buffer size
  int swap_w_ = 0, swap_h_ = 0;  // swap chain (window) size
  bool vsync_ = false;
  IDirect3DTexture9* staging_ = nullptr;
  IDirect3DSurface9* readback_ = nullptr;  // system-memory copy of the RT
  struct Read {
    Rect r{};
    std::vector<uint32_t> px;  // A8R8G8B8
  };
  std::map<uint32_t, Read> reads_;  // StartReadBack tickets
  uint32_t next_read_ = 1;
  Ring<IDirect3DVertexBuffer9> vb_;
  Ring<IDirect3DIndexBuffer9> ib_;
  struct MappedDraw {
    uint32_t nv, stride, ni, vofs = 0, iofs = 0;
  } mapped_{};
  std::map<uint32_t, IDirect3DTexture9*> textures_;
  std::map<uint32_t, IDirect3DCubeTexture9*> cubes_;  // same id space
  std::map<uint32_t, Target> targets_;  // render-target textures (default pool)
  std::map<uint32_t, Depth> depths_;
  uint32_t next_id_ = 1;
  TextureId current_color_ = 0;  // 0: the game's back buffer
  DepthId current_depth_ = 0;
  bool in_scene_ = false;
  std::set<uint32_t> ignored_rs_;

  // Everything set through the Device interface: D3D9's Reset discards
  // device state, and front ends only send state when it changes.
  struct State {
    std::map<uint32_t, uint32_t> rs;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> tss;
    std::map<uint32_t, TextureId> tex;
    std::map<uint32_t, std::array<float, 16>> xf;
    std::optional<Viewport> vp;
    std::optional<Rect> scissor;
    std::optional<Material> mat;
    std::map<uint32_t, Light> lights;
    std::map<uint32_t, bool> light_on;
    std::map<uint32_t, std::array<float, 4>> clip;
  } state_;

  void RestoreState() {
    const State s = state_;
    for (const auto& [k, v] : s.rs) SetRenderState(k, v);
    for (const auto& [k, v] : s.tss) SetTextureStageState(k.first, k.second, v);
    for (const auto& [k, v] : s.tex) SetTexture(k, v);
    for (const auto& [k, v] : s.xf) SetTransform(k, v.data());
    if (s.vp) SetViewport(*s.vp);
    if (s.scissor) SetScissor(*s.scissor);
    if (s.mat) SetMaterial(*s.mat);
    for (const auto& [k, v] : s.lights) SetLight(k, v);
    for (const auto& [k, v] : s.light_on) LightEnable(k, v);
    for (const auto& [k, v] : s.clip) SetClipPlane(k, v.data());
  }
};

}  // namespace

std::unique_ptr<Device> CreateD3D9Device(SDL_Window* window) {
  return std::make_unique<D3D9Device>(window);
}

}  // namespace recomp::gfx
