// Yuri's Revenge's settings, applied before the game starts (settings.cpp).
#pragma once

#include <string>

namespace yr {

// The screen size: "auto" (the default), "WxH" or "ini" (RA2MD.INI's own).
void SetResolution(const std::string& spec);

// RA2MD.INI (src/ra2/game_settings.h; the size changed as Red Alert 2's,
// src/ra2/resolution.h) and no startup movie. Main thread,
// before the game's entry point.
void ApplySettings();

}  // namespace yr
