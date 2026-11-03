// Frame presentation shared by the graphics APIs (ddraw, opengl32).

#include "runtime/hle/present.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime/hle/user32_window.h"
#include "runtime/log.h"
#include "runtime/runtime.h"
#include "runtime/watchdog.h"

namespace recomp::hle {

// Dev aid: <ID>_SHOT_EVERY=N saves every Nth presented frame (from frame
// <ID>_SHOT_FROM on) as $<ID>_SHOT_DIR/frame_NNNNN.bmp (default dir: current
// directory).
void PresentFrame(gfx::Device& dev, int w, int h) {
  static const int every = [] {
    const char* e = Env("SHOT_EVERY");
    return e ? std::atoi(e) : 0;
  }();
  static const int from = [] {
    const char* e = Env("SHOT_FROM");
    return e ? std::atoi(e) : 0;
  }();
  static int frame = 0;
  frame++;
  if (every > 0 && frame >= from && frame % every == 0) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    dev.ReadBack({0, 0, w, h}, px.data(), uint32_t(w * 4), gfx::Format::X8R8G8B8);
    SDL_Surface* surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888,
                                              px.data(), w * 4);
    const char* dir = Env("SHOT_DIR");
    char path[512];
    std::snprintf(path, sizeof path, "%s/frame_%05d.bmp", dir ? dir : ".", frame);
    if (surf) {
      SDL_SaveBMP(surf, path);
      SDL_DestroySurface(surf);
    }
  }
  dev.Present();
  // Loading screens present without pumping messages; keep answering the
  // window system (compositor pings) so the window does not look hung.
  // Events are only queued for the game, as Windows would.
  PumpHostEvents();
  WatchdogFrame();
  NoteFrameTiming();
}

void NoteFrameTiming() {
  static int frame = 0;
  frame++;
  // <ID>_HITCH=ms: report frames that took longer than that.
  static const double hitch_ms = [] {
    const char* e = Env("HITCH");
    return e ? std::atof(e) : 0.0;
  }();
  if (hitch_ms > 0) {
    static Uint64 last = 0, start = 0;
    Uint64 now = SDL_GetTicksNS();
    if (!start) start = now;
    double dt = last ? (now - last) / 1e6 : 0;
    if (dt > hitch_ms)
      WarnF("hitch: frame %d took %.1f ms (t=%.2f s)\n", frame, dt,
            (now - start) / 1e9);
    last = now;
  }
  // <ID>_FPS_REPORT=s: every s seconds, the frame rate and the spread of
  // frame intervals.
  static const double report_s = [] {
    const char* e = Env("FPS_REPORT");
    return e ? std::atof(e) : 0.0;
  }();
  if (report_s > 0) {
    static Uint64 last = 0, first = 0, t0 = 0;
    static std::vector<double> dts;
    Uint64 now = SDL_GetTicksNS();
    if (!t0) t0 = now;
    if (last) dts.push_back((now - last) / 1e6);
    if (!first) first = now;
    last = now;
    if ((now - first) / 1e9 >= report_s && !dts.empty()) {
      std::sort(dts.begin(), dts.end());
      auto q = [&](double p) { return dts[size_t(p * double(dts.size() - 1) + 0.5)]; };
      WarnF("fps: %.2f at t=%.1f s (interval ms: min %.3f p1 %.3f median %.3f p99 %.3f max %.3f)\n",
            dts.size() / ((now - first) / 1e9), (now - t0) / 1e9, dts.front(), q(0.01),
            q(0.5), q(0.99), dts.back());
      dts.clear();
      first = now;
    }
  }
}

gfx::Backend GfxBackend() {
  static const gfx::Backend b = [] {
    const char* e = Env("GFX");
    if (e && !std::strcmp(e, "gpu")) return gfx::Backend::Gpu;
    if (e && !std::strcmp(e, "d3d9")) {
#ifdef RECOMP_HAVE_D3D9
      return gfx::Backend::D3D9;
#else
      FatalF("graphics backend d3d9 is not in this build (no dxvk-native)");
#endif
    }
    if (e && *e) FatalF("unknown graphics backend '%s' (d3d9 or gpu)", e);
    return gfx::Backend::Gpu;
  }();
  return b;
}

uint64_t GfxWindowFlags() {
  switch (GfxBackend()) {
    case gfx::Backend::D3D9:
      return SDL_WINDOW_VULKAN;  // dxvk's SDL3 WSI makes a Vulkan surface
    case gfx::Backend::Gpu:
#if defined(__APPLE__)
      return SDL_WINDOW_METAL;
#else
      return SDL_WINDOW_VULKAN;
#endif
  }
  return 0;
}

std::unique_ptr<gfx::Device> CreateGfxDevice(SDL_Window* w) {
  std::unique_ptr<gfx::Device> d = gfx::CreateDevice(GfxBackend(), w);
  if (!d) FatalF("cannot create the graphics device");
  return d;
}

}  // namespace recomp::hle
