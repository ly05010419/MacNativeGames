// Backend selection and presentation geometry for gfx::Device.

#include "runtime/gfx/device.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace recomp::gfx {

namespace {
std::mutex g_present_mu;
Presentation g_present{};
}  // namespace

namespace {
float g_bar_r = 0, g_bar_g = 0, g_bar_b = 0;
Device* g_presenting = nullptr;
}  // namespace

Device* PresentingDevice() { return g_presenting; }
void SetPresentingDevice(Device* d) { g_presenting = d; }

void SetBarColor(float r, float g, float b) {
  g_bar_r = r;
  g_bar_g = g;
  g_bar_b = b;
}

void BarColor(float* r, float* g, float* b) {
  *r = g_bar_r;
  *g = g_bar_g;
  *b = g_bar_b;
}

Rect FitRect(int src_w, int src_h, int dst_w, int dst_h) {
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return {0, 0, dst_w, dst_h};
  int64_t w = dst_w, h = dst_h;
  if (int64_t(dst_w) * src_h > int64_t(dst_h) * src_w)
    w = (int64_t(dst_h) * src_w + src_h / 2) / src_h;  // narrower: pillarbox
  else
    h = (int64_t(dst_w) * src_h + src_w / 2) / src_w;  // wider: letterbox
  const int x = int(dst_w - w) / 2, y = int(dst_h - h) / 2;
  return {x, y, x + int(w), y + int(h)};
}

namespace {
SidePanel g_panel{};

bool PanelConfigured() { return g_panel.width > 0 && g_panel.scale > 1.0f; }

// How much wider the panel is shown than it is in the frame.
int PanelGrowth() {
  return PanelConfigured() ? int(std::lround(float(g_panel.width) * (g_panel.scale - 1.0f))) : 0;
}
}  // namespace

void SetSidePanel(const SidePanel& p) { g_panel = p; }

int g_crop_rows = 0;
bool (*g_crop_active)() = nullptr;

void SetCropBottom(int rows, bool (*active)()) {
  g_crop_rows = rows;
  g_crop_active = active;
}

int CropBottomShown() { return g_crop_rows > 0 && (!g_crop_active || g_crop_active()) ? g_crop_rows : 0; }

bool SidePanelShown() { return PanelConfigured() && g_panel.active && g_panel.active(); }

int ShownWidth(int src_w) { return src_w + PanelGrowth(); }

FrameLayout LayoutFrame(int src_w, int src_h, int dst_w, int dst_h, bool panel) {
  FrameLayout l;
  l.main_src = {0, 0, src_w, src_h};
  if (!panel || !PanelConfigured() || g_panel.width >= src_w) {
    const int crop = CropBottomShown();
    const int h = crop > 0 && crop < src_h ? src_h - crop : src_h;
    l.main_src = {0, 0, src_w, h};
    l.fit = l.main_dst = FitRect(src_w, h, dst_w, dst_h);
    return l;
  }
  const int main_w = src_w - g_panel.width;
  const int shown_w = ShownWidth(src_w);
  l.fit = FitRect(shown_w, src_h, dst_w, dst_h);
  const int fit_w = l.fit.right - l.fit.left;
  const int split = l.fit.left + int((int64_t(main_w) * fit_w + shown_w / 2) / shown_w);
  l.main_src = {0, 0, main_w, src_h};
  l.main_dst = {l.fit.left, l.fit.top, split, l.fit.bottom};
  l.panel = true;
  const int rows = std::max(1, int(std::lround(float(src_h) / g_panel.scale)));
  l.panel_src = {main_w, 0, src_w, std::min(rows, src_h)};
  l.panel_dst = {split, l.fit.top, l.fit.right, l.fit.bottom};
  const int bar_h = std::min(g_panel.bar_height, src_h);
  if (bar_h > 0 && g_panel.bar_scale > 1.0f) {
    // The main part's own scale (picture pixels per frame pixel) times the bar's.
    const float k = float(split - l.fit.left) / float(main_w) * g_panel.bar_scale;
    const int bar_w = std::clamp(int(std::lround(float(main_w) * g_panel.bar_length)), 1, main_w);
    l.bar = true;
    l.bar_src = {0, src_h - bar_h, bar_w, src_h};
    l.bar_dst = {l.fit.left, l.fit.bottom - int(std::lround(float(bar_h) * k)),
                 std::min(split, l.fit.left + int(std::lround(float(bar_w) * k))), l.fit.bottom};
  }
  return l;
}

void NotePresentation(const Presentation& p) {
  std::lock_guard<std::mutex> l(g_present_mu);
  g_present = p;
}

Presentation LastPresentation() {
  std::lock_guard<std::mutex> l(g_present_mu);
  return g_present;
}

std::unique_ptr<Device> CreateDevice(Backend b, SDL_Window* window) {
  switch (b) {
    case Backend::D3D9:
#ifdef RECOMP_HAVE_D3D9
      return CreateD3D9Device(window);
#else
      return nullptr;
#endif
    case Backend::Gpu:
      return CreateGpuDevice(window);
  }
  return nullptr;
}

}  // namespace recomp::gfx
