// Where the project's code finds the game's own variables and functions:
// Red Alert 2 1.006 (GAME.EXE) and Yuri's Revenge 1.001 (gamemd.exe) have
// the same code at moved addresses. The running game's table is chosen by
// its RunGame (src/app/games.h) before anything reads it; an app may hold
// both games (iOS), so the choice is made at run time.
#pragma once

#include <cstdint>

namespace ra2 {

struct GameAddresses {
  // Set while the battlefield's main loop runs a frame (the loop sets it
  // on entry, clears it on its way out): 1 only in a match, not in menus,
  // the loading screen or the in-match options (observed in .data dumps).
  uint32_t in_main_loop;
  // The tactical (battlefield) view object's pointer.
  uint32_t tactical_ptr;
  // The view's map-bounds clamp (stdcall, one Point2D*; true when it
  // clamped) and the recomputing of its corner after a move (thiscall).
  uint32_t clamp_to_map, view_moved;
  // Set when the view's update skips the clamp's result.
  uint32_t unclamped;
  // The tactical view's rectangle: y and height (Set_View_Dimensions).
  uint32_t view_y, view_h;
  // The .data section (development dumps, devtools.cpp).
  uint32_t data_va, data_size;
};

// Observed in each game's code (Yuri's Revenge's matched instruction by
// instruction with Red Alert 2's: the limiter, the main loop, the view's
// update, the sidebar's layout).
extern const GameAddresses kRa2Addresses;
extern const GameAddresses kYrAddresses;

const GameAddresses& Addresses();
void UseAddresses(const GameAddresses& a);

}  // namespace ra2
