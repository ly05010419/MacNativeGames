// SDL2's windows, events, keyboard, mouse and game controllers on SDL3
// (see sdl2.cpp).
// SDL2 windows are HWNDs too (CreateSdl2Window), so the HWND a game gets
// from SDL_GetWindowWMInfo works with DXGI and user32. Window coordinates
// and sizes are pixels, as SDL2 reports them to a DPI-aware Windows
// program; SDL3 works in points, converted with the window's pixel
// density. A guest SDL_Window* is an opaque heap block.
//
// Events: once SDL2's video is up, the host event stream is SDL2's
// (SetHostEventSink): each SDL3 event becomes the SDL2 event(s) it stands
// for, in SDL2's x64 layout (a 56-byte union, millisecond timestamps), or
// is dropped when SDL2 has no such event (pens, cameras, SDL3-only window
// and device events) or disables it by default (drag and drop, which a
// game enables with SDL_EventState).

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_window.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "sdl2.dll";

uint64_t Bool(bool b) { return b ? 1 : 0; }

// ---- windows ----------------------------------------------------------------------

struct Sdl2Window {
  SDL_Window* sdl;
  uint32_t hwnd;
  uint32_t flags;  // SDL2 flags given at creation that SDL3 does not keep
};

std::mutex g_win_mu;
std::map<gaddr, Sdl2Window> g_windows;  // by guest SDL_Window*

Sdl2Window Must(gaddr w, const char* api) {
  std::lock_guard<std::mutex> l(g_win_mu);
  auto it = g_windows.find(w);
  if (it == g_windows.end()) FatalF("%s: unknown SDL_Window %08llX", api, (unsigned long long)w);
  return it->second;
}

int ToPoints(SDL_Window* w, int px) {
  float d = SDL_GetWindowPixelDensity(w);
  return d > 0 ? int(std::lround(px / d)) : px;
}
int ToPixels(SDL_Window* w, float pt) {
  float d = SDL_GetWindowPixelDensity(w);
  return int(std::floor(pt * (d > 0 ? d : 1)));
}

// SDL2 window flags.
enum : uint32_t {
  kFullscreen = 0x1, kOpenGl = 0x2, kShown = 0x4, kHidden = 0x8, kBorderless = 0x10,
  kResizable = 0x20, kMinimized = 0x40, kMaximized = 0x80, kMouseGrabbed = 0x100,
  kInputFocus = 0x200, kMouseFocus = 0x400, kFullscreenDesktop = 0x1001,
  kAllowHighDpi = 0x2000, kMouseCapture = 0x4000, kAlwaysOnTop = 0x8000,
  kKeyboardGrabbed = 0x100000,
};

// ---- events -----------------------------------------------------------------------

using Event = std::array<uint8_t, 56>;  // SDL2's SDL_Event

std::mutex g_ev_mu;
std::deque<Event> g_events;

// SDL2 event types.
enum : uint32_t {
  kQuit = 0x100, kDisplayEvent = 0x150, kWindowEvent = 0x200, kKeyDown = 0x300,
  kKeyUp = 0x301, kTextEditing = 0x302, kTextInput = 0x303, kKeymapChanged = 0x304,
  kMouseMotion = 0x400, kMouseButtonDown = 0x401, kMouseButtonUp = 0x402, kMouseWheel = 0x403,
  kJoyAxis = 0x600, kJoyBall = 0x601, kJoyHat = 0x602, kJoyButtonDown = 0x603,
  kJoyButtonUp = 0x604, kJoyDeviceAdded = 0x605, kJoyDeviceRemoved = 0x606,
  kJoyBattery = 0x607, kPadAxis = 0x650, kPadButtonDown = 0x651, kPadButtonUp = 0x652,
  kPadAdded = 0x653, kPadRemoved = 0x654, kPadRemapped = 0x655, kPadTouchDown = 0x656,
  kPadTouchMotion = 0x657, kPadTouchUp = 0x658, kPadSensor = 0x659, kClipboard = 0x900,
};

// SDL2's joystick device indices (SDL_NumJoysticks, SDL_GameControllerOpen,
// the which of the ADDED events): the connected joysticks in the order
// they appeared. SDL3 has instance IDs only.
std::mutex g_dev_mu;
std::vector<SDL_JoystickID> g_devices;

// Brings the list up to date with the connected joysticks: SDL2 counts
// devices as the driver sees them, before their events are pumped.
void SyncDevices() {
  int n = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&n);
  std::lock_guard<std::mutex> l(g_dev_mu);
  std::erase_if(g_devices, [&](SDL_JoystickID d) { return !std::count(ids, ids + n, d); });
  for (int i = 0; i < n; i++)
    if (!std::count(g_devices.begin(), g_devices.end(), ids[i])) g_devices.push_back(ids[i]);
  SDL_free(ids);
}

int DeviceIndex(SDL_JoystickID id) {
  std::lock_guard<std::mutex> l(g_dev_mu);
  for (size_t i = 0; i < g_devices.size(); i++)
    if (g_devices[i] == id) return int(i);
  return -1;
}

// The joystick at SDL2 device index i, or 0.
SDL_JoystickID DeviceAt(int i) {
  SyncDevices();
  std::lock_guard<std::mutex> l(g_dev_mu);
  return i >= 0 && size_t(i) < g_devices.size() ? g_devices[i] : 0;
}

struct Builder {
  Event e{};
  Builder(uint32_t type, uint64_t timestamp_ns) {
    Put32(0, type);
    Put32(4, uint32_t(timestamp_ns / 1000000));
  }
  void Put8(int o, uint8_t v) { e[o] = v; }
  void Put16(int o, uint16_t v) { std::memcpy(&e[o], &v, 2); }
  void Put32(int o, uint32_t v) { std::memcpy(&e[o], &v, 4); }
  void PutF(int o, float v) { std::memcpy(&e[o], &v, 4); }
  void Push() {
    std::lock_guard<std::mutex> l(g_ev_mu);
    g_events.push_back(e);
  }
};

// SDL3 window events with an SDL2 SDL_WINDOWEVENT counterpart: its event id.
uint8_t WindowEventId(uint32_t t) {
  switch (t) {
    case SDL_EVENT_WINDOW_SHOWN: return 1;
    case SDL_EVENT_WINDOW_HIDDEN: return 2;
    case SDL_EVENT_WINDOW_EXPOSED: return 3;
    case SDL_EVENT_WINDOW_MOVED: return 4;
    case SDL_EVENT_WINDOW_RESIZED: return 5;
    case SDL_EVENT_WINDOW_MINIMIZED: return 7;
    case SDL_EVENT_WINDOW_MAXIMIZED: return 8;
    case SDL_EVENT_WINDOW_RESTORED: return 9;
    case SDL_EVENT_WINDOW_MOUSE_ENTER: return 10;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: return 11;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: return 12;
    case SDL_EVENT_WINDOW_FOCUS_LOST: return 13;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return 14;
    case SDL_EVENT_WINDOW_HIT_TEST: return 16;
    case SDL_EVENT_WINDOW_ICCPROF_CHANGED: return 17;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: return 18;
    default: return 0;
  }
}

int DisplayIndex(SDL_DisplayID id) {
  int n = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&n);
  int r = -1;
  for (int i = 0; i < n; i++)
    if (ids[i] == id) r = i;
  SDL_free(ids);
  return r;
}

// SDL2 keycodes are SDL3's except for SDL3's extended ones (keys SDL2
// cannot name: SDLK_UNKNOWN); SDL3's KMOD_LEVEL5 does not exist in SDL2.
uint32_t Keycode(SDL_Keycode k) { return k & SDLK_EXTENDED_MASK ? 0 : k; }
uint16_t Keymod(SDL_Keymod m) { return uint16_t(m & ~SDL_KMOD_LEVEL5); }

// Relative mouse motion in pixels: SDL3's float deltas, with the
// fractions carried so slow motion is not lost.
float g_rel_x = 0, g_rel_y = 0;

// Text in SDL2's 32-byte event buffers: at most 31 bytes, split on UTF-8
// sequence boundaries.
std::vector<std::string> TextChunks(const char* text) {
  std::vector<std::string> out;
  std::string s = text ? text : "";
  size_t i = 0;
  while (i < s.size()) {
    size_t n = std::min<size_t>(31, s.size() - i);
    while (n && i + n < s.size() && (uint8_t(s[i + n]) & 0xC0) == 0x80) n--;
    out.push_back(s.substr(i, n));
    i += n;
  }
  return out;
}

void Convert(const SDL_Event& e) {
  const uint64_t ts = e.common.timestamp;
  switch (e.type) {
    case SDL_EVENT_QUIT: Builder(kQuit, ts).Push(); return;
    case SDL_EVENT_TERMINATING: case SDL_EVENT_LOW_MEMORY: case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND: case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND: case SDL_EVENT_LOCALE_CHANGED:
      Builder(e.type, ts).Push();  // the same numbers in SDL2, no payload
      return;
    case SDL_EVENT_KEYMAP_CHANGED: Builder(kKeymapChanged, ts).Push(); return;
    case SDL_EVENT_CLIPBOARD_UPDATE: Builder(kClipboard, ts).Push(); return;
    case SDL_EVENT_DISPLAY_ORIENTATION: case SDL_EVENT_DISPLAY_ADDED:
    case SDL_EVENT_DISPLAY_REMOVED: case SDL_EVENT_DISPLAY_MOVED: {
      Builder b(kDisplayEvent, ts);
      b.Put32(8, uint32_t(DisplayIndex(e.display.displayID)));
      b.Put8(12, uint8_t(e.type - SDL_EVENT_DISPLAY_ORIENTATION + 1));
      b.Put32(16, uint32_t(e.display.data1));
      b.Push();
      return;
    }
    default: break;
  }
  if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) {
    uint8_t id = WindowEventId(e.type);
    if (!id) return;
    SDL_Window* w = SDL_GetWindowFromID(e.window.windowID);
    int32_t d1 = e.window.data1, d2 = e.window.data2;
    if (e.type == SDL_EVENT_WINDOW_RESIZED && w) {
      d1 = ToPixels(w, float(d1));
      d2 = ToPixels(w, float(d2));
    } else if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
      d1 = DisplayIndex(SDL_DisplayID(d1));
    }
    auto push = [&](uint8_t ev) {
      Builder b(kWindowEvent, ts);
      b.Put32(8, e.window.windowID);
      b.Put8(12, ev);
      b.Put32(16, uint32_t(d1));
      b.Put32(20, uint32_t(d2));
      b.Push();
    };
    // SDL2 follows every size change with SIZE_CHANGED.
    push(id);
    if (id == 5) push(6);
    return;
  }
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
      Builder b(e.key.down ? kKeyDown : kKeyUp, ts);
      b.Put32(8, e.key.windowID);
      b.Put8(12, e.key.down);
      b.Put8(13, e.key.repeat);
      b.Put32(16, uint32_t(e.key.scancode));
      b.Put32(20, Keycode(e.key.key));
      b.Put16(24, Keymod(e.key.mod));
      b.Push();
      return;
    }
    case SDL_EVENT_TEXT_INPUT:
      for (const std::string& t : TextChunks(e.text.text)) {
        Builder b(kTextInput, ts);
        b.Put32(8, e.text.windowID);
        std::memcpy(&b.e[12], t.data(), t.size());
        b.Push();
      }
      return;
    case SDL_EVENT_TEXT_EDITING: {
      std::vector<std::string> t = TextChunks(e.edit.text);
      Builder b(kTextEditing, ts);
      b.Put32(8, e.edit.windowID);
      if (!t.empty()) std::memcpy(&b.e[12], t[0].data(), t[0].size());
      b.Put32(44, uint32_t(e.edit.start));
      b.Put32(48, uint32_t(e.edit.length));
      b.Push();
      return;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      SDL_Window* w = SDL_GetWindowFromID(e.motion.windowID);
      float d = w ? SDL_GetWindowPixelDensity(w) : 1;
      g_rel_x += e.motion.xrel * d;
      g_rel_y += e.motion.yrel * d;
      int32_t rx = int32_t(std::trunc(g_rel_x)), ry = int32_t(std::trunc(g_rel_y));
      g_rel_x -= float(rx);
      g_rel_y -= float(ry);
      Builder b(kMouseMotion, ts);
      b.Put32(8, e.motion.windowID);
      b.Put32(12, e.motion.which);
      b.Put32(16, e.motion.state);
      b.Put32(20, uint32_t(int32_t(std::floor(e.motion.x * d))));
      b.Put32(24, uint32_t(int32_t(std::floor(e.motion.y * d))));
      b.Put32(28, uint32_t(rx));
      b.Put32(32, uint32_t(ry));
      b.Push();
      return;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
      SDL_Window* w = SDL_GetWindowFromID(e.button.windowID);
      Builder b(e.button.down ? kMouseButtonDown : kMouseButtonUp, ts);
      b.Put32(8, e.button.windowID);
      b.Put32(12, e.button.which);
      b.Put8(16, e.button.button);
      b.Put8(17, e.button.down);
      b.Put8(18, e.button.clicks);
      b.Put32(20, uint32_t(w ? ToPixels(w, e.button.x) : int(e.button.x)));
      b.Put32(24, uint32_t(w ? ToPixels(w, e.button.y) : int(e.button.y)));
      b.Push();
      return;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      SDL_Window* w = SDL_GetWindowFromID(e.wheel.windowID);
      Builder b(kMouseWheel, ts);
      b.Put32(8, e.wheel.windowID);
      b.Put32(12, e.wheel.which);
      b.Put32(16, uint32_t(e.wheel.integer_x));
      b.Put32(20, uint32_t(e.wheel.integer_y));
      b.Put32(24, uint32_t(e.wheel.direction));
      b.PutF(28, e.wheel.x);
      b.PutF(32, e.wheel.y);
      b.Put32(36, uint32_t(w ? ToPixels(w, e.wheel.mouse_x) : int(e.wheel.mouse_x)));
      b.Put32(40, uint32_t(w ? ToPixels(w, e.wheel.mouse_y) : int(e.wheel.mouse_y)));
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_ADDED: {
      {
        std::lock_guard<std::mutex> l(g_dev_mu);
        if (!std::count(g_devices.begin(), g_devices.end(), e.jdevice.which))
          g_devices.push_back(e.jdevice.which);
      }
      Builder b(kJoyDeviceAdded, ts);
      b.Put32(8, uint32_t(DeviceIndex(e.jdevice.which)));
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_REMOVED: {
      {
        std::lock_guard<std::mutex> l(g_dev_mu);
        std::erase(g_devices, e.jdevice.which);
      }
      Builder b(kJoyDeviceRemoved, ts);
      b.Put32(8, e.jdevice.which);
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
      Builder b(kJoyAxis, ts);
      b.Put32(8, e.jaxis.which);
      b.Put8(12, e.jaxis.axis);
      b.Put16(16, uint16_t(e.jaxis.value));
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_BALL_MOTION: {
      Builder b(kJoyBall, ts);
      b.Put32(8, e.jball.which);
      b.Put8(12, e.jball.ball);
      b.Put16(16, uint16_t(e.jball.xrel));
      b.Put16(18, uint16_t(e.jball.yrel));
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
      Builder b(kJoyHat, ts);
      b.Put32(8, e.jhat.which);
      b.Put8(12, e.jhat.hat);
      b.Put8(13, e.jhat.value);
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP: {
      Builder b(e.jbutton.down ? kJoyButtonDown : kJoyButtonUp, ts);
      b.Put32(8, e.jbutton.which);
      b.Put8(12, e.jbutton.button);
      b.Put8(13, e.jbutton.down);
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_BATTERY_UPDATED: {
      // SDL_JoystickPowerLevel: unknown -1, empty, low, medium, full, wired.
      int level = e.jbattery.state == SDL_POWERSTATE_CHARGING ||
                          e.jbattery.state == SDL_POWERSTATE_CHARGED
                      ? 5
                  : e.jbattery.percent < 0   ? -1
                  : e.jbattery.percent <= 5  ? 0
                  : e.jbattery.percent <= 20 ? 1
                  : e.jbattery.percent <= 70 ? 2
                                             : 3;
      Builder b(kJoyBattery, ts);
      b.Put32(8, e.jbattery.which);
      b.Put32(12, uint32_t(level));
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      Builder b(kPadAxis, ts);
      b.Put32(8, e.gaxis.which);
      b.Put8(12, e.gaxis.axis);
      b.Put16(16, uint16_t(e.gaxis.value));
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
      // SDL2's button numbers are SDL3's (A = south, ..., the paddles in
      // the same order).
      Builder b(e.gbutton.down ? kPadButtonDown : kPadButtonUp, ts);
      b.Put32(8, e.gbutton.which);
      b.Put8(12, e.gbutton.button);
      b.Put8(13, e.gbutton.down);
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_ADDED: {
      Builder b(kPadAdded, ts);
      b.Put32(8, uint32_t(DeviceIndex(e.gdevice.which)));
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: case SDL_EVENT_GAMEPAD_REMAPPED: {
      Builder b(e.type == SDL_EVENT_GAMEPAD_REMOVED ? kPadRemoved : kPadRemapped, ts);
      b.Put32(8, e.gdevice.which);
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN: case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP: {
      Builder b(kPadTouchDown + (e.type - SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN), ts);
      b.Put32(8, e.gtouchpad.which);
      b.Put32(12, uint32_t(e.gtouchpad.touchpad));
      b.Put32(16, uint32_t(e.gtouchpad.finger));
      b.PutF(20, e.gtouchpad.x);
      b.PutF(24, e.gtouchpad.y);
      b.PutF(28, e.gtouchpad.pressure);
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
      Builder b(kPadSensor, ts);
      b.Put32(8, e.gsensor.which);
      b.Put32(12, uint32_t(e.gsensor.sensor));
      for (int i = 0; i < 3; i++) b.PutF(16 + 4 * i, e.gsensor.data[i]);
      uint64_t us = e.gsensor.sensor_timestamp / 1000;
      std::memcpy(&b.e[32], &us, 8);
      b.Push();
      return;
    }
    default:
      return;  // no SDL2 counterpart, or disabled by default in SDL2
  }
}

bool g_events_up = false;

// SDL2's video is up: its events from now on (main thread).
void TakeHostEvents() {
  if (g_events_up) return;
  g_events_up = true;
  SetHostEventSink(Convert);
}

}  // namespace

// ---- windows --------------------------------------------------------------------------

// SDL_CreateWindow(title, x, y, w, h, flags). Fullscreen and OpenGL
// windows are not provided yet.
HLE64(kDll, SDL_CreateWindow, kCdecl) {
  std::string title = Arg(c, 0) ? GStr(Arg(c, 0)) : "";
  int w = int(int32_t(Arg(c, 3))), h = int(int32_t(Arg(c, 4)));
  uint32_t flags = uint32_t(Arg(c, 5));
  RECOMP_TRACE("  SDL_CreateWindow(\"%s\", %d, %d, %dx%d, flags %X)\n", title.c_str(),
               int(int32_t(Arg(c, 1))), int(int32_t(Arg(c, 2))), w, h, flags);
  if (flags & (kFullscreen | kOpenGl))
    FatalF("SDL_CreateWindow: flags %X (fullscreen/OpenGL windows not provided yet)", flags);
  SDL_WindowFlags f = 0;
  if (flags & kHidden || !(flags & kShown)) f |= SDL_WINDOW_HIDDEN;
  if (flags & kBorderless) f |= SDL_WINDOW_BORDERLESS;
  if (flags & kResizable) f |= SDL_WINDOW_RESIZABLE;
  if (flags & kMinimized) f |= SDL_WINDOW_MINIMIZED;
  if (flags & kMaximized) f |= SDL_WINDOW_MAXIMIZED;
  if (flags & kAlwaysOnTop) f |= SDL_WINDOW_ALWAYS_ON_TOP;
  TakeHostEvents();
  SDL_Window* sdl;
  uint32_t hwnd = CreateSdl2Window(title, w, h, f, &sdl);
  if (!hwnd) return 0;
  gaddr p = HeapAllocProcess(16, true);
  std::lock_guard<std::mutex> l(g_win_mu);
  g_windows[p] = {sdl, hwnd, flags & kAllowHighDpi};
  return p;
}

HLE64(kDll, SDL_DestroyWindow, kCdecl) {
  Sdl2Window w = Must(Arg(c, 0), "SDL_DestroyWindow");
  {
    std::lock_guard<std::mutex> l(g_win_mu);
    g_windows.erase(Arg(c, 0));
  }
  DestroySdl2Window(w.hwnd);
  HeapFreeProcess(Arg(c, 0));
  return 0;
}

HLE64(kDll, SDL_ShowWindow, kCdecl) {
  SDL_ShowWindow(Must(Arg(c, 0), "SDL_ShowWindow").sdl);
  return 0;
}
HLE64(kDll, SDL_HideWindow, kCdecl) {
  SDL_HideWindow(Must(Arg(c, 0), "SDL_HideWindow").sdl);
  return 0;
}
HLE64(kDll, SDL_RaiseWindow, kCdecl) {
  if (!Background()) SDL_RaiseWindow(Must(Arg(c, 0), "SDL_RaiseWindow").sdl);
  return 0;
}
HLE64(kDll, SDL_MinimizeWindow, kCdecl) {
  SDL_MinimizeWindow(Must(Arg(c, 0), "SDL_MinimizeWindow").sdl);
  return 0;
}
HLE64(kDll, SDL_SetWindowTitle, kCdecl) {
  SDL_SetWindowTitle(Must(Arg(c, 0), "SDL_SetWindowTitle").sdl, GStr(Arg(c, 1)).c_str());
  return 0;
}

// The client area in pixels.
HLE64(kDll, SDL_GetWindowSize, kCdecl) {
  SDL_Window* w = Must(Arg(c, 0), "SDL_GetWindowSize").sdl;
  int pw = 0, ph = 0;
  SDL_GetWindowSizeInPixels(w, &pw, &ph);
  if (Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(pw));
  if (Arg(c, 2)) Wr32(Arg(c, 2), uint32_t(ph));
  return 0;
}
HLE64(kDll, SDL_SetWindowSize, kCdecl) {
  SDL_Window* w = Must(Arg(c, 0), "SDL_SetWindowSize").sdl;
  SDL_SetWindowSize(w, ToPoints(w, int(int32_t(Arg(c, 1)))), ToPoints(w, int(int32_t(Arg(c, 2)))));
  return 0;
}

HLE64(kDll, SDL_GetWindowFlags, kCdecl) {
  Sdl2Window w = Must(Arg(c, 0), "SDL_GetWindowFlags");
  SDL_WindowFlags f = SDL_GetWindowFlags(w.sdl);
  uint32_t r = w.flags;
  if (f & SDL_WINDOW_FULLSCREEN)
    r |= SDL_GetWindowFullscreenMode(w.sdl) ? kFullscreen : kFullscreenDesktop;
  r |= f & SDL_WINDOW_HIDDEN ? kHidden : kShown;
  if (f & SDL_WINDOW_BORDERLESS) r |= kBorderless;
  if (f & SDL_WINDOW_RESIZABLE) r |= kResizable;
  if (f & SDL_WINDOW_MINIMIZED) r |= kMinimized;
  if (f & SDL_WINDOW_MAXIMIZED) r |= kMaximized;
  if (f & SDL_WINDOW_MOUSE_GRABBED) r |= kMouseGrabbed;
  if (f & SDL_WINDOW_INPUT_FOCUS) r |= kInputFocus;
  if (f & SDL_WINDOW_MOUSE_FOCUS) r |= kMouseFocus;
  if (f & SDL_WINDOW_MOUSE_CAPTURE) r |= kMouseCapture;
  if (f & SDL_WINDOW_ALWAYS_ON_TOP) r |= kAlwaysOnTop;
  if (f & SDL_WINDOW_KEYBOARD_GRABBED) r |= kKeyboardGrabbed;
  return r;
}

// SDL_GetWindowWMInfo(window, SDL_SysWMinfo*): {SDL_version (3 bytes),
// subsystem @4, HWND @8, HDC @16, HINSTANCE @24} for SDL_SYSWM_WINDOWS.
HLE64(kDll, SDL_GetWindowWMInfo, kCdecl) {
  Sdl2Window w = Must(Arg(c, 0), "SDL_GetWindowWMInfo");
  gaddr info = Arg(c, 1);
  if (Rd8(info) != 2) {
    SDL_SetError("Application not compiled with SDL 2");
    return 0;
  }
  Wr32(info + 4, 1);  // SDL_SYSWM_WINDOWS
  Wr64(info + 8, w.hwnd);
  Wr64(info + 16, DcForWindow(w.hwnd));
  Wr64(info + 24, g_proc->img.image_base);
  return 1;
}

// ---- events ---------------------------------------------------------------------------

HLE64(kDll, SDL_PumpEvents, kCdecl) {
  PumpHostEvents();
  return 0;
}

// SDL_PollEvent(event): pumps, then takes the next event (with a null
// event, only tells whether there is one).
HLE64(kDll, SDL_PollEvent, kCdecl) {
  PumpHostEvents();
  std::lock_guard<std::mutex> l(g_ev_mu);
  if (g_events.empty()) return 0;
  if (gaddr out = Arg(c, 0)) {
    std::memcpy(GPtr(out), g_events.front().data(), 56);
    g_events.pop_front();
  }
  return 1;
}

// ---- game controllers ---------------------------------------------------------------
// A guest SDL_GameController* or SDL_Joystick* is a heap block standing for
// the host gamepad; a controller's joystick (SDL_GameControllerGetJoystick)
// is one block per controller, as SDL2 returns the same pointer each time.

namespace {

struct Pad {
  SDL_Gamepad* pad;
  gaddr joystick;  // its SDL_Joystick* block
  gaddr name;      // guest copy of the name (SDL_GameControllerName)
};

std::mutex g_pad_mu;
std::map<gaddr, Pad> g_pads;           // by SDL_GameController*
std::map<gaddr, SDL_Joystick*> g_joys;  // by SDL_Joystick*

SDL_Gamepad* PadOf(gaddr gc, const char* api) {
  std::lock_guard<std::mutex> l(g_pad_mu);
  auto it = g_pads.find(gc);
  if (it == g_pads.end()) FatalF("%s: unknown SDL_GameController %08llX", api, (unsigned long long)gc);
  return it->second.pad;
}

SDL_Joystick* JoyOf(gaddr j, const char* api) {
  std::lock_guard<std::mutex> l(g_pad_mu);
  auto it = g_joys.find(j);
  if (it == g_joys.end()) FatalF("%s: unknown SDL_Joystick %08llX", api, (unsigned long long)j);
  return it->second;
}

}  // namespace

HLE64(kDll, SDL_NumJoysticks, kCdecl) {
  SyncDevices();
  std::lock_guard<std::mutex> l(g_dev_mu);
  return g_devices.size();
}

HLE64(kDll, SDL_IsGameController, kCdecl) {
  SDL_JoystickID id = DeviceAt(int(int32_t(Arg(c, 0))));
  return Bool(id && SDL_IsGamepad(id));
}

HLE64(kDll, SDL_JoystickGetDeviceInstanceID, kCdecl) {
  SDL_JoystickID id = DeviceAt(int(int32_t(Arg(c, 0))));
  return id ? id : uint32_t(-1);
}

HLE64(kDll, SDL_GameControllerOpen, kCdecl) {
  SDL_JoystickID id = DeviceAt(int(int32_t(Arg(c, 0))));
  SDL_Gamepad* pad = id ? SDL_OpenGamepad(id) : nullptr;
  if (!pad) {
    if (!id) SDL_SetError("There are %d joysticks available", int(g_devices.size()));
    return 0;
  }
  // SDL2 hands out one object per device, reference counted.
  std::lock_guard<std::mutex> l(g_pad_mu);
  for (auto& [gc, p] : g_pads)
    if (p.pad == pad) return gc;
  gaddr gc = HeapAllocProcess(16, true), j = HeapAllocProcess(16, true);
  const char* name = SDL_GetGamepadName(pad);
  std::string n = name ? name : "";
  gaddr gname = HeapAllocProcess(n.size() + 1, false);
  PutStr(gname, n);
  g_pads[gc] = {pad, j, gname};
  g_joys[j] = SDL_GetGamepadJoystick(pad);
  return gc;
}

HLE64(kDll, SDL_GameControllerClose, kCdecl) {
  gaddr gc = Arg(c, 0);
  if (!gc) return 0;
  SDL_Gamepad* pad = PadOf(gc, "SDL_GameControllerClose");
  SDL_CloseGamepad(pad);
  // The last close of the device frees it (SDL3 counts opens too).
  if (SDL_GetGamepadFromID(SDL_GetGamepadID(pad)) == pad) return 0;
  std::lock_guard<std::mutex> l(g_pad_mu);
  Pad p = g_pads[gc];
  g_joys.erase(p.joystick);
  g_pads.erase(gc);
  HeapFreeProcess(p.joystick);
  HeapFreeProcess(p.name);
  HeapFreeProcess(gc);
  return 0;
}

HLE64(kDll, SDL_GameControllerGetAttached, kCdecl) {
  return Bool(SDL_GamepadConnected(PadOf(Arg(c, 0), "SDL_GameControllerGetAttached")));
}

// SDL2's axis numbers are SDL3's.
HLE64(kDll, SDL_GameControllerGetAxis, kCdecl) {
  return uint16_t(SDL_GetGamepadAxis(PadOf(Arg(c, 0), "SDL_GameControllerGetAxis"),
                                     SDL_GamepadAxis(int32_t(Arg(c, 1)))));
}

HLE64(kDll, SDL_GameControllerGetJoystick, kCdecl) {
  PadOf(Arg(c, 0), "SDL_GameControllerGetJoystick");
  std::lock_guard<std::mutex> l(g_pad_mu);
  return g_pads[Arg(c, 0)].joystick;
}

HLE64(kDll, SDL_JoystickInstanceID, kCdecl) {
  SDL_JoystickID id = SDL_GetJoystickID(JoyOf(Arg(c, 0), "SDL_JoystickInstanceID"));
  return id ? id : uint32_t(-1);
}

HLE64(kDll, SDL_GameControllerName, kCdecl) {
  PadOf(Arg(c, 0), "SDL_GameControllerName");
  std::lock_guard<std::mutex> l(g_pad_mu);
  return g_pads[Arg(c, 0)].name;
}

HLE64(kDll, SDL_GameControllerGetVendor, kCdecl) {
  return SDL_GetGamepadVendor(PadOf(Arg(c, 0), "SDL_GameControllerGetVendor"));
}
HLE64(kDll, SDL_GameControllerGetProduct, kCdecl) {
  return SDL_GetGamepadProduct(PadOf(Arg(c, 0), "SDL_GameControllerGetProduct"));
}

// Sensor types are numbered alike (accelerometer 1, gyroscope 2, ...).
HLE64(kDll, SDL_GameControllerHasSensor, kCdecl) {
  return Bool(SDL_GamepadHasSensor(PadOf(Arg(c, 0), "SDL_GameControllerHasSensor"),
                                   SDL_SensorType(int32_t(Arg(c, 1)))));
}
HLE64(kDll, SDL_GameControllerSetSensorEnabled, kCdecl) {
  return SDL_SetGamepadSensorEnabled(PadOf(Arg(c, 0), "SDL_GameControllerSetSensorEnabled"),
                                     SDL_SensorType(int32_t(Arg(c, 1))), uint32_t(Arg(c, 2)) != 0)
             ? 0
             : uint32_t(-1);
}
HLE64(kDll, SDL_GameControllerGetSensorData, kCdecl) {
  int n = int(int32_t(Arg(c, 3)));
  std::vector<float> v(size_t(std::max(n, 0)));
  if (!SDL_GetGamepadSensorData(PadOf(Arg(c, 0), "SDL_GameControllerGetSensorData"),
                                SDL_SensorType(int32_t(Arg(c, 1))), v.data(), n))
    return uint32_t(-1);
  std::memcpy(GPtr(Arg(c, 2)), v.data(), 4 * v.size());
  return 0;
}

HLE64(kDll, SDL_JoystickIsHaptic, kCdecl) {
  return SDL_IsJoystickHaptic(JoyOf(Arg(c, 0), "SDL_JoystickIsHaptic")) ? 1 : 0;
}

}  // namespace recomp::hle
