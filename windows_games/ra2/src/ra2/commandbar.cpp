// The command bar below the battlefield (sidebar.h, kCommandBarHeight)
// left out: the game runs as it would on a screen the bar's lines taller
// (resolution.cpp), with the bar at its bottom, and in a match those lines
// are not shown (GameConfig::crop_bottom, main.cpp): the battlefield fills
// the screen and the pointer cannot reach the bar. The game's layout is
// untouched.

#include "commandbar.h"

namespace ra2 {
namespace {

bool g_hidden = false;

}  // namespace

void SetCommandBarHidden(bool hidden) { g_hidden = hidden; }
bool CommandBarHidden() { return g_hidden; }

}  // namespace ra2
