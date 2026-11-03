// The play options and touch controls both games share (controls.cpp).
#pragma once

#include "runtime/runtime.h"

namespace ra2 {

// The defaults (on a phone, no command bar: the on-screen buttons stand in
// for it and the battlefield takes its lines) and the environment's
// <prefix>_SIDEBAR_SCALE, <prefix>_FPS_CAP and <prefix>_COMMAND_BAR=0.
void ReadPlayOptions(const char* prefix);

// --fps-cap <n> (frame_pacing.h), --sidebar-scale <s> (sidebar.h),
// --no-command-bar (commandbar.h) at argv[*i], advancing *i past a value:
// true if it was one of them.
bool PlayOption(int argc, char** argv, int* i);

// The touch screen's controls (gestures, the stick that moves the view,
// the buttons, only in a match) and what goes with them: the command bar's
// lines cropped when it is left out, the sidebar enlarged when asked, the
// frame-rate counter and menus shown at the battlefield's rate.
void AddPlayControls(recomp::GameConfig& g);

}  // namespace ra2
