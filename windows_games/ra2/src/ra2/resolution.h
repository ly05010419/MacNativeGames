// The game's screen size (RA2.INI [Video] ScreenWidth/ScreenHeight), set
// before it starts. The game reads it once and runs the whole session in
// that DirectDraw mode; its menus (800x600) are centred in it, the battle
// view fills it.
#pragma once

#include <cstdint>
#include <string>

namespace ra2 {

// "auto": the screen's own shape at 768 lines, 512 on a phone (a phone or
// a wide monitor is filled without stretching); "WxH": that size; "ini": as the player's
// RA2.INI says. Takes effect in ApplyResolution. With the sidebar enlarged
// (sidebar.h) the game's mode is narrower than that by the sidebar's growth.
void SetResolution(const std::string& spec);

// The project's changes to a chosen screen size, in either game: narrower
// by the enlarged sidebar's growth (sidebar.h); without the command bar,
// its lines below the screen, out of view (commandbar.h).
void AdjustScreen(int* w, int* h);

// Writes the chosen size into the game's RA2.INI (the runtime's writable
// copy of it). Main thread, before the game's entry point.
void ApplyResolution();

}  // namespace ra2
