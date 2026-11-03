// The sidebar's layout (Red Alert 2: thiscall, no arguments, 0x67B910; its
// one caller, 0x67BAD0, runs when a match's display is set up; Yuri's
// Revenge: 0x6A5130, the same computation with a left-hand sidebar too;
// replaced in hooks.cpp and src/yr/hooks.cpp) takes its height from the
// tactical view's rectangle
// (Set_View_Dimensions, 0x839630: x, y, w, h; at 768 lines 0,0,W-168,736):
//
//   strip top    0x839624 = 158 (below the radar and the tabs)
//   strip height 0x83962C = 32 (0x7ADD84) + view y + view h - 158
//   strip rows   0xABCD70 = (height - 18 or 26 - 76) / 50 * 50
//
// and everything the sidebar draws, lays its buttons out by and clips to
// comes from those (the drawing at 0x67CF80 repeats SIDE2.SHP per 50-pixel
// row, then SIDE3/the bottom). The hook runs the layout with the view's
// height cut to what a screen 1/scale as tall would have, and puts the
// height back: the battlefield keeps its size, the sidebar fits in the
// screen's top 1/scale.

#include "sidebar.h"

#include "addresses.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "runtime/runtime.h"

namespace ra2 {
namespace {

// How long the sidebar counts as shown after the loop was last seen
// running: frames presented between two of its frames keep the panel.
constexpr auto kShownGrace = std::chrono::milliseconds(150);
// The strip's height formula's lines below the view (Red Alert 2's
// 0x7ADD84, Yuri's Revenge's 0x7F5BFC, both 32): the command bar's,
// whether or not it is shown.
constexpr int32_t kBelowView = kCommandBarHeight;
// The shortest screen the layout keeps one row of build icons on: the
// strip's top, its largest top and bottom margins and one 50-line row.
constexpr int32_t kMinScreenH = 158 + 26 + 76 + 50;

float g_scale = 1.0f;

}  // namespace

void SetSidebarScale(float scale) { g_scale = scale; }
float SidebarScale() { return g_scale; }

bool SidebarShown() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last_seen{};
  const Clock::time_point now = Clock::now();
  if (recomp::Rd8(Addresses().in_main_loop)) last_seen = now;
  const bool shown = last_seen != Clock::time_point{} && now - last_seen < kShownGrace;
  static bool was = false;
  if (shown != was) {
    was = shown;
    std::fprintf(stderr, "ra2: sidebar %s\n", shown ? "shown" : "hidden");
  }
  return shown;
}

void LayoutSidebar(recomp::Ctx& c, void (*layout)(recomp::Ctx&)) {
  const float scale = SidebarScale();
  const uint32_t view_y = Addresses().view_y, view_h = Addresses().view_h;
  const int32_t y = int32_t(recomp::Rd32(view_y));
  const int32_t h = int32_t(recomp::Rd32(view_h));
  // The screen's height: the view and the command bar below it.
  const int32_t screen_h = y + h + kCommandBarHeight;
  int32_t laid_out = screen_h;
  if (scale > 1.0f)
    laid_out = std::min(screen_h, std::max(kMinScreenH, int32_t(float(screen_h) / scale)));
  const int32_t layout_h = laid_out - kBelowView - y;
  if (layout_h == h) {
    layout(c);
    return;
  }
  recomp::Wr32(view_h, uint32_t(layout_h));
  layout(c);
  recomp::Wr32(view_h, uint32_t(h));
  std::fprintf(stderr, "ra2: sidebar laid out for %d of %d lines (scale %.2f)\n", laid_out,
               screen_h, double(scale));
}

}  // namespace ra2
