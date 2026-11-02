#pragma once
// Host-side keyboard/mouse state shared between the SDL event pump and the
// user32 HLE (GetKeyState, GetAsyncKeyState, GetCursorPos).

#include <atomic>
#include <cstdint>

namespace recomp {

extern std::atomic<uint8_t> g_key_down[256];
extern std::atomic<uint8_t> g_key_toggled[256];
extern std::atomic<uint8_t> g_key_pressed_since[256];

// Screen size reported to the game (GetSystemMetrics, EnumDisplaySettings):
// the desktop, or the display mode the game switched to, in pixels. Set
// from the primary display once SDL is up; the default is a safe
// 2002-era desktop.
inline int g_display_w = 1024;
inline int g_display_h = 768;
// The host desktop's size in pixels (DesktopSize, display.h) and its
// pixel density (pixels per point); fixed once SDL is up.
inline int g_desktop_w = 1024;
inline int g_desktop_h = 768;
inline float g_desktop_density = 1;

// Set-1 scan code for a virtual key (US layout); *ext = E0-prefixed.
uint8_t ScanCodeForVk(uint8_t vk, bool* ext);

// Raw input as DirectInput reports it, from the SDL event pump (main
// thread): mouse motion in mickeys (host relative motion, unscaled: one
// unit of host motion is one mickey, as a device unit is on Windows), the
// wheel in WHEEL_DELTA units, mouse buttons 0-4 (left, right, middle, X1,
// X2) and keys by DIK code (set-1 scan code, 0x80 for E0-prefixed ones).
struct RawInput {
  enum Kind : uint8_t { MouseX, MouseY, Wheel, MouseButton, Key };
  Kind kind;
  uint8_t code;      // button or DIK code
  int32_t value;     // motion/wheel delta; 1/0 for down/up
  uint32_t time_ms;  // GetTickCount time
};
using RawInputSink = void (*)(const RawInput&);
// One sink (the DirectInput HLE); null to stop.
void SetRawInputSink(RawInputSink sink);
void EmitRawInput(RawInput::Kind kind, uint8_t code, int32_t value);

}  // namespace recomp
