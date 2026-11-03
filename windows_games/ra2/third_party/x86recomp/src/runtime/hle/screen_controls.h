#pragma once
// On-screen controls drawn over the picture (gfx overlay), for games
// played on a screen without a mouse at hand (GameConfig::show_fps,
// touch_stick):
//   - the frame rate (frames shown per second) at the top left;
//   - a fixed stick at the bottom left, always shown: held off centre, it
//     scrolls the view in its direction (one of eight), by holding the
//     pointer at that edge of the screen for games that scroll there;
//   - round buttons on an arc around it (GameConfig::screen_buttons).
// Fingers (or, on a desktop, the mouse) that land on the stick drive it
// and reach nothing else.

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>

namespace recomp {

// The stick's direction for a deflection (vx, vy) in units of its radius:
// each of dx, dy is -1, 0 or 1 (y down); 0, 0 inside the dead zone. Eight
// sectors of 45 degrees.
void StickDirection(float vx, float vy, int* dx, int* dy);

// The stick's deflection (vx, vy) in units of its radius (y down), and
// whether it is held. For GameConfig::Stick::Query.
bool StickDeflection(float* vx, float* vy);

// Where the stick sends the pointer: dx, dy as StickDirection gives them,
// 0, 0 when it is let go. Set by the window system.
using StickEdgeFn = void (*)(int dx, int dy);
void SetStickEdgeHandler(StickEdgeFn fn);

// What happens to the buttons around the stick (GameConfig::screen_buttons),
// by index: a tap (RightClick, Key) or a double tap (Key), each followed for
// a Key by TapEnd or DoubleTapEnd shortly after (the game has read the key
// with its modifier held: let go of it); a StickyKey button arming and
// letting go. Set by the window system.
enum class ButtonEvent { Tap, TapEnd, DoubleTap, DoubleTapEnd, Arm, Disarm };
using ScreenButtonFn = void (*)(size_t index, ButtonEvent ev);
void SetScreenButtonHandler(ScreenButtonFn fn);

// A click outside the controls has been made: armed StickyKey buttons let
// go shortly after (once the game has read the click with the key held).
void ScreenButtonsClicked();

// Once per event pump: layout for `window`, the frame rate, the overlay.
void ScreenControlsTick(SDL_Window* window);

// A pointer (finger id, or the mouse) at x, y in the drawable's pixels.
// True when the controls take it (it then reaches nothing else).
bool ScreenControlsDown(uint64_t id, float x, float y);
bool ScreenControlsMove(uint64_t id, float x, float y);
bool ScreenControlsUp(uint64_t id);

}  // namespace recomp
