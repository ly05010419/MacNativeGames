#include "resolution.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "commandbar.h"
#include "game_settings.h"
#include "sidebar.h"

namespace ra2 {
namespace {

std::string g_spec = "auto";

// With the sidebar enlarged (sidebar.h) the window is wider than the
// game's mode by the sidebar's growth: the mode is that much narrower than
// the screen it fills, kept a multiple of 8 and no narrower than the menus.
int ModeWidth(int screen_w) {
  const float s = SidebarScale();
  if (s <= 1.0f) return screen_w;
  const int growth = int(std::lround(float(kSidebarWidth) * (s - 1.0f)));
  return std::max(kMenuWidth, int(std::lround((screen_w - growth) / 8.0)) * 8);
}

}  // namespace

void AdjustScreen(int* w, int* h) {
  *w = ModeWidth(*w);
  if (CommandBarHidden()) *h += kCommandBarHeight;
}

void SetResolution(const std::string& spec) { g_spec = spec; }

void ApplyResolution() { ApplyGameSettings("RA2.INI", g_spec, "ra2", AdjustScreen); }

}  // namespace ra2
