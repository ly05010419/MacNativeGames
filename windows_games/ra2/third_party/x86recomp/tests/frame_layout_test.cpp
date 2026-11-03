// How frames are laid out in the output (gfx::LayoutFrame, gfx::SidePanel):
// the whole frame fitted, or the main part and an enlarged side panel. The
// expected rectangles are worked out by hand from the definitions in
// runtime/gfx/device.h.

#include <cstdio>

#include "runtime/gfx/device.h"

namespace {

using recomp::gfx::FrameLayout;
using recomp::gfx::LayoutFrame;
using recomp::gfx::Rect;
using recomp::gfx::SetSidePanel;
using recomp::gfx::ShownWidth;
using recomp::gfx::SidePanel;
using recomp::gfx::SidePanelShown;

int g_failures = 0;

void ExpectRect(const Rect& got, const Rect& want, const char* what) {
  if (got.left != want.left || got.top != want.top || got.right != want.right ||
      got.bottom != want.bottom) {
    std::printf("FAIL: %s\n  want: %d,%d,%d,%d\n  got:  %d,%d,%d,%d\n", what, want.left,
                want.top, want.right, want.bottom, got.left, got.top, got.right, got.bottom);
    g_failures++;
  }
}

void Expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
  }
}

bool Yes() { return true; }
bool No() { return false; }

void NoPanel() {
  SetSidePanel({});
  // 1024x768 in a 2048x1536 output: scaled 2x, the whole output.
  const FrameLayout l = LayoutFrame(1024, 768, 2048, 1536, true);
  Expect(!l.panel, "no panel configured: none shown");
  ExpectRect(l.fit, {0, 0, 2048, 1536}, "no panel: fit");
  ExpectRect(l.main_src, {0, 0, 1024, 768}, "no panel: main_src is the frame");
  ExpectRect(l.main_dst, {0, 0, 2048, 1536}, "no panel: main_dst is the fit");
  Expect(ShownWidth(1024) == 1024, "no panel: shown width is the frame's");
  Expect(!SidePanelShown(), "no panel: never shown");
}

void Panel() {
  // A 168-wide panel at 1.5x grows by 84: a 1032x512 frame shows 1116 wide.
  SetSidePanel({168, 1.5f, Yes});
  Expect(ShownWidth(1032) == 1116, "panel: shown width 1032 + 84");
  Expect(SidePanelShown(), "panel: active says yes");

  // The output is exactly the shown size: main part 1:1 (0..864), the
  // panel's top round(512 / 1.5) = 341 rows fill 864..1116 x 0..512.
  FrameLayout l = LayoutFrame(1032, 512, 1116, 512, true);
  Expect(l.panel, "panel: shown");
  ExpectRect(l.fit, {0, 0, 1116, 512}, "panel 1x: fit");
  ExpectRect(l.main_src, {0, 0, 864, 512}, "panel 1x: main_src");
  ExpectRect(l.main_dst, {0, 0, 864, 512}, "panel 1x: main_dst");
  ExpectRect(l.panel_src, {864, 0, 1032, 341}, "panel 1x: panel_src");
  ExpectRect(l.panel_dst, {864, 0, 1116, 512}, "panel 1x: panel_dst");

  // Twice the size: split at 864 * 2232 / 1116 = 1728.
  l = LayoutFrame(1032, 512, 2232, 1024, true);
  ExpectRect(l.fit, {0, 0, 2232, 1024}, "panel 2x: fit");
  ExpectRect(l.main_dst, {0, 0, 1728, 1024}, "panel 2x: main_dst");
  ExpectRect(l.panel_dst, {1728, 0, 2232, 1024}, "panel 2x: panel_dst");

  // A taller output: letterboxed, (1024 - 512) / 2 = 256 above and below.
  l = LayoutFrame(1032, 512, 1116, 1024, true);
  ExpectRect(l.fit, {0, 256, 1116, 768}, "panel letterbox: fit");
  ExpectRect(l.panel_dst, {864, 256, 1116, 768}, "panel letterbox: panel_dst");

  // Not shown (menus): the whole frame pillarboxed in the wider output,
  // (1116 - 1032) / 2 = 42 each side.
  l = LayoutFrame(1032, 512, 1116, 512, false);
  Expect(!l.panel, "panel off: not shown");
  ExpectRect(l.fit, {42, 0, 1074, 512}, "panel off: fit");
  ExpectRect(l.main_src, {0, 0, 1032, 512}, "panel off: main_src is the frame");
  ExpectRect(l.main_dst, {42, 0, 1074, 512}, "panel off: main_dst");
  Expect(!l.bar, "panel without a bar: none");

  // With a bar: the bottom 32 rows, the left half of the 864-wide main part
  // (432), twice as large: 864x64 over the main part's bottom left.
  SetSidePanel({168, 1.5f, Yes, 32, 0.5f, 2.0f});
  l = LayoutFrame(1032, 512, 1116, 512, true);
  Expect(l.bar, "bar: shown with the panel");
  ExpectRect(l.bar_src, {0, 480, 432, 512}, "bar 1x: bar_src");
  ExpectRect(l.bar_dst, {0, 448, 864, 512}, "bar 1x: bar_dst");
  ExpectRect(l.main_dst, {0, 0, 864, 512}, "bar 1x: main_dst unchanged");
  // Output twice the size: the main part at 2x, the bar at 4x: 1728x128.
  l = LayoutFrame(1032, 512, 2232, 1024, true);
  ExpectRect(l.bar_dst, {0, 896, 1728, 1024}, "bar 2x: bar_dst");
  // Not shown without the panel.
  l = LayoutFrame(1032, 512, 1116, 512, false);
  Expect(!l.bar, "bar: not shown without the panel");

  SetSidePanel({168, 1.5f, No});
  Expect(!SidePanelShown(), "panel: active says no");
  SetSidePanel({168, 1.5f, nullptr});
  Expect(!SidePanelShown(), "panel: no active callback");
  SetSidePanel({168, 1.0f, Yes});
  Expect(!SidePanelShown(), "panel: scale 1 is no panel");
  Expect(ShownWidth(1032) == 1032, "panel: scale 1 does not widen");
  SetSidePanel({});
}

}  // namespace

int main() {
  NoPanel();
  Panel();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("frame_layout: all passed\n");
  return 0;
}
