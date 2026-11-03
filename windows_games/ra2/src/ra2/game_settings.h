// The project's settings in the game's own INI file, written before the
// game starts (game_settings.cpp): shared by Red Alert 2 (RA2.INI) and
// Yuri's Revenge (RA2MD.INI), which read the same keys.
#pragma once

#include <string>

namespace ra2 {

// The menus' width (their 800x600 artwork): the least a mode may be.
constexpr int kMenuWidth = 800;

// In the runtime's writable copy of `ini_name` (in the game directory):
//   - [Intro] Play=no: a fresh install plays its four-minute intro once,
//     which a touch screen cannot skip (the project leaves movies out);
//   - once (marked [WinNativeGames] DefaultsApplied=1, so the player's own
//     choice is kept afterwards): GameSpeed=1 in [Options] and
//     [Skirmish], shown as 5, a frame every 16 ms tick (the Steam install
//     says 3, about 20 frames a second);
//   - [Video] ScreenWidth/ScreenHeight from `spec`: "auto" (the screen's
//     own shape at 768 lines, 512 on a phone), "WxH", or "ini" (left as
//     the file says); `adjust`, if any, may change the size first.
// `tag` names the game in messages. Main thread, before the entry point.
void ApplyGameSettings(const std::string& ini_name, const std::string& spec, const char* tag,
                       void (*adjust)(int* w, int* h));

}  // namespace ra2
