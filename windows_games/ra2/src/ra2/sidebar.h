// The sidebar laid out for a shorter screen (sidebar.cpp): with a scale s,
// its build strip has the rows a screen 1/s as tall would give it, so the
// sidebar's top 1/s of the screen holds all of it and can be shown s times
// as large while the battlefield stays at 1x.
#pragma once

namespace recomp {
struct Ctx;
}

namespace ra2 {

// 1 (the default) leaves the game's own layout. Read when a match's
// sidebar is laid out; set before the game starts.
void SetSidebarScale(float scale);
float SidebarScale();

// The sidebar's width in the game's screen (0x7ADD80).
constexpr int kSidebarWidth = 168;
// The command bar below the battlefield (its buttons: groups, formation,
// scatter, guard, waypoints): the screen's bottom 32 lines (0x7ADD84)
// left of the sidebar, its buttons all in the left half. Shown with the
// enlarged sidebar: that half, twice as large.
constexpr int kCommandBarHeight = 32;
constexpr float kCommandBarLength = 0.5f;
constexpr float kCommandBarScale = 2.0f;

// Whether the sidebar is on screen (the battlefield is shown, not a menu
// or the in-match options): the runtime then shows it enlarged
// (GameConfig::side_panel). Asked from the presenting thread.
bool SidebarShown();

// The game's sidebar layout, run through this from its replacement
// (`layout`: its lifted code; hooks.cpp, src/yr/hooks.cpp): with the view's
// height cut to what a screen 1/scale as tall would have, put back after.
void LayoutSidebar(recomp::Ctx& c, void (*layout)(recomp::Ctx&));

}  // namespace ra2
