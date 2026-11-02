// user32: window classes, windows, message queues and the SDL3 event pump.
//
// Every top-level window gets an SDL window sized to its client area; child
// windows are bookkeeping only. SDL events are translated into WM_* messages
// on the queue of the thread that owns the window, and DispatchMessage calls
// the guest WndProc. Messages Windows sends synchronously (creation,
// activation, size) are delivered by calling the WndProc directly.

#include <SDL3/SDL.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "runtime/display.h"
#include "runtime/gfx/device.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/input_state.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/present.h"
#include "runtime/hle/screen_controls.h"
#include "runtime/hle/touch_gestures.h"
#include "runtime/hle/user32_internal.h"
#include "runtime/hle/user32_window.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/platform.h"
#include "runtime/watchdog.h"

namespace recomp {

// SDL_MouseID of <ID>_TEST_INPUT "pointer" events: they stand for the host
// pointer, which automated (background) runs otherwise ignore.
constexpr SDL_MouseID kTestPointer = 0xD51D51;
// SDL_MouseID of the mouse events touch gestures make (GameConfig::Touch::
// Pointer), and the SDL_TouchID of <ID>_TEST_INPUT "touch" events (a
// direct touch screen; their positions are in window coordinates).
constexpr SDL_MouseID kTouchPointer = 0xD51D52;
constexpr SDL_TouchID kTestTouch = 0xD51D53;

// Pointer events that give an absolute position the cursor goes to, even
// a virtual one, and that background runs accept.
bool AbsolutePointer(SDL_MouseID which) { return which == kTestPointer || which == kTouchPointer; }

bool Background() {
  static const bool bg = [] {
    const char* e = Env("BACKGROUND");
    return e && *e && *e != '0';
  }();
  return bg;
}

namespace user32 {

std::recursive_mutex g_mu;
std::map<std::string, WndClass> g_classes;
std::map<uint32_t, Window> g_windows;
std::map<uint32_t, Queue> g_queues;  // by tid
std::vector<Timer> g_timers;

Clip g_clip;
uint32_t g_capture = 0;  // SetCapture

std::vector<HotKey> g_hotkeys;
std::condition_variable_any g_queue_cv;
uint32_t g_next_hwnd = 0x00010010;
uint16_t g_next_atom = 0xC100;
uint32_t g_focus = 0, g_active = 0;
int g_cursor_count = 0;  // ShowCursor display counter
int32_t g_mouse_x = 0, g_mouse_y = 0;  // screen coordinates
// The host pointer over the game window, in client (mode) coordinates;
// valid while g_pointer_inside. Written on the main thread.
std::atomic<bool> g_pointer_inside{false};
std::atomic<int32_t> g_real_x{0}, g_real_y{0};
// When the game last re-centred the cursor (it does so every frame while
// it steers its own pointer by relative motion).
std::atomic<int64_t> g_last_recentre_ms{-100000};
std::atomic<int32_t> g_recentre_x{0}, g_recentre_y{0};  // screen coordinates
bool g_sdl_up = false;
std::thread::id g_main_thread_id = std::this_thread::get_id();

Window* Win(uint32_t hwnd) {
  auto it = g_windows.find(hwnd);
  return it == g_windows.end() || it->second.destroyed ? nullptr : &it->second;
}

// Frame sizes for a top-level style: border on each side, caption on top.
void FrameSize(uint32_t style, int* border, int* caption) {
  bool framed = style & (WS_CAPTION | WS_THICKFRAME);
  *border = !framed ? 0 : (style & WS_THICKFRAME) ? 4 : 3;
  *caption = framed && (style & WS_CAPTION) == WS_CAPTION ? 19 : 0;
}

// Client-area size for a window rect, mirroring what AdjustWindowRect added.
void ClientSize(const Window& w, int* cw, int* ch) {
  int border = 0, caption = 0;
  if (!(w.style & WS_CHILD)) FrameSize(w.style, &border, &caption);
  *cw = std::max(0, w.w - 2 * border);
  *ch = std::max(0, w.h - 2 * border - caption);
}

void PostTo(uint32_t tid, const Msg& m) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    g_queues[tid].msgs.push_back(m);
  }
  g_queue_cv.notify_all();
}

void Post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  uint32_t tid;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    tid = w ? w->tid : CurrentThread()->tid;
  }
  PostTo(tid, Msg{hwnd, msg, wp, lp, TickCount(), g_mouse_x, g_mouse_y});
}

// Synchronous delivery to the guest WndProc (SendMessage semantics).
uint32_t Send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  uint32_t proc;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (w->tid != CurrentThread()->tid)
      WarnF("SendMessage %X to hwnd %X owned by thread %X from thread %X: "
            "delivered on the calling thread\n",
            msg, hwnd, w->tid, CurrentThread()->tid);
    proc = w->wndproc;
  }
  Ctx& c = CurrentThread()->ctx;
  return CallGuest(c, proc, {hwnd, msg, wp, lp});
}

uint32_t MkLparam(int lo, int hi) {
  return (uint32_t(uint16_t(lo))) | (uint32_t(uint16_t(hi)) << 16);
}

// ---- the window tree -------------------------------------------------------------

std::vector<uint32_t> Children(uint32_t parent) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  std::vector<std::pair<int64_t, uint32_t>> v;
  for (auto& [h, w] : g_windows)
    if (!w.destroyed && (w.style & WS_CHILD) && w.parent == parent) v.push_back({w.z, h});
  std::sort(v.begin(), v.end());
  std::vector<uint32_t> out;
  for (auto& [z, h] : v) out.push_back(h);
  return out;
}

uint32_t TopLevel(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (Window* w = Win(hwnd); w && (w->style & WS_CHILD) && Win(w->parent); w = Win(w->parent))
    hwnd = w->parent;
  return hwnd;
}

bool IsSelfOrAncestor(uint32_t ancestor, uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (Window* w = Win(hwnd); w; w = (w->style & WS_CHILD) ? Win(w->parent) : nullptr) {
    if (hwnd == ancestor) return true;
    hwnd = w->parent;
  }
  return false;
}

// Controls whose WM_NCHITTEST is HTTRANSPARENT: a static control without
// SS_NOTIFY and a group box. Hit testing goes on to the windows below.
bool HitTransparent(const Window& w) {
  if (w.cls == "static") return !(w.style & 0x100);
  if (w.cls == "button") return (w.style & 0xF) == 7;
  return false;
}

uint32_t ChildAtScreen(uint32_t hwnd, int sx, int sy) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (;;) {
    uint32_t hit = 0;
    for (uint32_t ch : Children(hwnd)) {
      Window* w = Win(ch);
      if (!w || !w->visible || HitTransparent(*w)) continue;
      int ox, oy;
      WindowOrigin(ch, &ox, &oy);  // children have no frame: origin = window rect
      if (sx >= ox && sy >= oy && sx < ox + w->w && sy < oy + w->h) {
        hit = ch;
        break;
      }
    }
    if (!hit) return hwnd;
    hwnd = hit;
  }
}

// Whether hwnd and all its parents are visible (it can be painted).
bool ShownOnScreen(uint32_t hwnd) {
  for (Window* w = Win(hwnd); w; w = (w->style & WS_CHILD) ? Win(w->parent) : nullptr)
    if (!w->visible) return false;
  return Win(hwnd) != nullptr;
}

void Invalidate(uint32_t hwnd, const int* r, bool erase) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w) return;
  int cw, ch;
  ClientSize(*w, &cw, &ch);
  int a[4] = {0, 0, cw, ch};
  if (r) {
    a[0] = std::max(r[0], 0);
    a[1] = std::max(r[1], 0);
    a[2] = std::min(r[2], cw);
    a[3] = std::min(r[3], ch);
  }
  if (a[0] >= a[2] || a[1] >= a[3]) return;
  if (!w->dirty) {
    w->ul = a[0], w->ut = a[1], w->ur = a[2], w->ub = a[3];
  } else {
    w->ul = std::min(w->ul, a[0]), w->ut = std::min(w->ut, a[1]);
    w->ur = std::max(w->ur, a[2]), w->ub = std::max(w->ub, a[3]);
  }
  w->dirty = true;
  w->erase = w->erase || erase;
  g_queue_cv.notify_all();
}

void InvalidateTree(uint32_t hwnd) {
  Invalidate(hwnd, nullptr, true);
  for (uint32_t ch : Children(hwnd)) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(ch);
    if (w && w->visible) InvalidateTree(ch);
  }
}

// A WM_PAINT sent here (not dispatched from the queue) that the window
// procedure returns from without BeginPaint or ValidateRect leaves no
// update region behind: the system validates the window itself, as
// Windows does for a paint it sent that was not processed. (A queued
// WM_PAINT that is not validated keeps coming back.)
void PaintNow(uint32_t hwnd) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w || !w->dirty || !ShownOnScreen(hwnd)) return;
    w->paint_sent = true;
  }
  Send(hwnd, WM_PAINT, 0, 0);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd); w && w->paint_sent) {
    w->paint_sent = false;
    w->dirty = w->erase = false;
  }
}

// Top-level window under SDL window id.
uint32_t HwndForSdl(SDL_WindowID id) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows)
    if (w.sdl && SDL_GetWindowID(w.sdl) == id && !w.destroyed) return h;
  return 0;
}

// ---- SDL key -> Windows VK ----------------------------------------------------

uint8_t VkFromScancode(SDL_Scancode sc) {
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return uint8_t('A' + (sc - SDL_SCANCODE_A));
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return uint8_t('1' + (sc - SDL_SCANCODE_1));
  if (sc == SDL_SCANCODE_0) return '0';
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return uint8_t(0x70 + (sc - SDL_SCANCODE_F1));
  switch (sc) {
    case SDL_SCANCODE_RETURN: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PRINTSCREEN: return 0x2C;
    case SDL_SCANCODE_SCROLLLOCK: return 0x91;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_KP_1: return 0x61;
    case SDL_SCANCODE_KP_2: return 0x62;
    case SDL_SCANCODE_KP_3: return 0x63;
    case SDL_SCANCODE_KP_4: return 0x64;
    case SDL_SCANCODE_KP_5: return 0x65;
    case SDL_SCANCODE_KP_6: return 0x66;
    case SDL_SCANCODE_KP_7: return 0x67;
    case SDL_SCANCODE_KP_8: return 0x68;
    case SDL_SCANCODE_KP_9: return 0x69;
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_LCTRL: return 0xA2;
    case SDL_SCANCODE_RCTRL: return 0xA3;
    case SDL_SCANCODE_LSHIFT: return 0xA0;
    case SDL_SCANCODE_RSHIFT: return 0xA1;
    case SDL_SCANCODE_LALT: return 0xA4;
    case SDL_SCANCODE_RALT: return 0xA5;
    case SDL_SCANCODE_LGUI: return 0x5B;
    case SDL_SCANCODE_RGUI: return 0x5C;
    case SDL_SCANCODE_APPLICATION: return 0x5D;
    default: return 0;
  }
}

// Messages use the generic VK for modifiers; key state tracks both.
uint8_t GenericVk(uint8_t vk) {
  switch (vk) {
    case 0xA0: case 0xA1: return 0x10;
    case 0xA2: case 0xA3: return 0x11;
    case 0xA4: case 0xA5: return 0x12;
    default: return vk;
  }
}

uint32_t MouseKeys() {
  uint32_t mk = 0;
  if (g_key_down[0x01]) mk |= 0x01;  // MK_LBUTTON
  if (g_key_down[0x02]) mk |= 0x02;  // MK_RBUTTON
  if (g_key_down[0x10]) mk |= 0x04;  // MK_SHIFT
  if (g_key_down[0x11]) mk |= 0x08;  // MK_CONTROL
  if (g_key_down[0x04]) mk |= 0x10;  // MK_MBUTTON
  return mk;
}

void SetKey(uint8_t vk, bool down) {
  if (down && !g_key_down[vk]) {
    g_key_pressed_since[vk] = 1;
    g_key_toggled[vk] = !g_key_toggled[vk];
  }
  g_key_down[vk] = down;
}

void WindowOrigin(uint32_t hwnd, int* x, int* y) {
  *x = *y = 0;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd)) {
    if ((w->style & WS_CHILD) && Win(w->parent)) {
      WindowOrigin(w->parent, x, y);  // a child has no frame here
      *x += w->x;
      *y += w->y;
      return;
    }
    int cw, ch;
    ClientSize(*w, &cw, &ch);
    *x = w->x + (w->w - cw) / 2;
    *y = w->y + (w->h - ch) - (w->w - cw) / 2;
  }
}

// True (with the mode size) while hwnd is in DirectDraw exclusive mode.
bool ExclusiveSize(uint32_t hwnd, int* w, int* h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* win = Win(hwnd);
  if (!win || !win->mode_w) return false;
  *w = win->mode_w;
  *h = win->mode_h;
  return true;
}

// The client area as the game sees it: the display mode in exclusive mode.
void GameClientSize(const Window& w, int* cw, int* ch) {
  if (w.mode_w) {
    *cw = w.mode_w;
    *ch = w.mode_h;
  } else {
    ClientSize(w, cw, ch);
  }
}

// Host window coordinates -> client coordinates: client = (host - x0) * k.
// The frame (the client area) is shown fitted into the host window with
// its aspect kept (gfx::LayoutFrame, as present does), so when the host
// window has another size (fullscreen, or resized by the window manager or
// by desktop scaling) points map through that rectangle. `fitted` is false
// when the two have the same size (k = 1, no offset). While the last frame
// was shown with the side panel enlarged (gfx::SidePanel), x0/k map the
// main part and points from px rightwards map through the panel's own
// rectangle: client = (pcx + (host - px) * pkx, (host - y0) * pky); points
// over the enlarged bar (drawn over the main part, from host (bx0, by0) to
// (bx1, by1)) through its own: client = ((host - bx0) * bkx,
// bcy + (host - by0) * bky).
struct ClientMap {
  float kx = 1, ky = 1, x0 = 0, y0 = 0;
  int cw = 0, ch = 0;
  bool fitted = false;
  bool panel = false;
  float px = 0, pkx = 1, pky = 1;
  int pcx = 0;
  bool bar = false;
  float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0, bkx = 1, bky = 1;
  int bcy = 0;

  void ToClient(float hx, float hy, int* x, int* y) const {
    if (bar && hx >= bx0 && hx < bx1 && hy >= by0 && hy < by1) {
      *x = int((hx - bx0) * bkx);
      *y = bcy + int((hy - by0) * bky);
      return;
    }
    if (panel && hx >= px) {
      *x = pcx + int((hx - px) * pkx);
      *y = int((hy - y0) * pky);
      return;
    }
    *x = int((hx - x0) * kx);
    *y = int((hy - y0) * ky);
  }
};
ClientMap MapToClient(uint32_t hwnd) {
  ClientMap m;
  SDL_Window* sdl;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w || !w->sdl) return m;
    sdl = w->sdl;
    GameClientSize(*w, &m.cw, &m.ch);
  }
  const bool panel = gfx::LastPresentation().panel;
  int ww = 0, wh = 0;
  if (!SDL_GetWindowSize(sdl, &ww, &wh) || ww <= 0 || wh <= 0 || m.cw <= 0 || m.ch <= 0 ||
      (ww == m.cw && wh == m.ch && !panel && !gfx::CropBottomShown()))
    return m;
  const gfx::FrameLayout lay = gfx::LayoutFrame(m.cw, m.ch, ww, wh, panel);
  const gfx::Rect& r = lay.main_dst;
  m.kx = float(lay.main_src.right - lay.main_src.left) / float(r.right - r.left);
  m.ky = float(lay.main_src.bottom - lay.main_src.top) / float(r.bottom - r.top);
  // A cropped frame: the pointer stays in the rows shown.
  if (!lay.panel) m.ch = lay.main_src.bottom - lay.main_src.top;
  m.x0 = float(r.left);
  m.y0 = float(r.top);
  m.fitted = true;
  if (lay.panel) {
    const gfx::Rect& ps = lay.panel_src;
    const gfx::Rect& pd = lay.panel_dst;
    m.panel = true;
    m.px = float(pd.left);
    m.pkx = float(ps.right - ps.left) / float(pd.right - pd.left);
    m.pky = float(ps.bottom - ps.top) / float(pd.bottom - pd.top);
    m.pcx = ps.left;
  }
  if (lay.bar) {
    const gfx::Rect& bs = lay.bar_src;
    const gfx::Rect& bd = lay.bar_dst;
    m.bar = true;
    m.bx0 = float(bd.left);
    m.by0 = float(bd.top);
    m.bx1 = float(bd.right);
    m.by1 = float(bd.bottom);
    m.bkx = float(bs.right - bs.left) / float(bd.right - bd.left);
    m.bky = float(bs.bottom - bs.top) / float(bd.bottom - bd.top);
    m.bcy = bs.top;
  }
  return m;
}

// The window whose client area holds the clip rectangle, or 0.
uint32_t ClipWindow() {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows) {
    if (w.destroyed || !w.sdl) continue;
    int ox, oy, cw, ch;
    WindowOrigin(h, &ox, &oy);
    ClientSize(w, &cw, &ch);
    if (g_clip.l >= ox && g_clip.t >= oy && g_clip.r <= ox + cw + 1 &&
        g_clip.b <= oy + ch + 1)
      return h;
  }
  return 0;
}

// A clip is a screen rectangle and outlives the window it was set for, as
// on Windows: a game that recreates its window (e.g. to restart its
// renderer) without clipping again stays confined. Once a window covers
// the rectangle again, its cursor is virtual and the host pointer grabbed.
void AttachClip() {
  SDL_Window* grab = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!g_clip.on || g_clip.hwnd) return;
    g_clip.hwnd = ClipWindow();
    if (Window* w = Win(g_clip.hwnd)) grab = w->sdl;
  }
  if (grab && !Background()) SDL_SetWindowRelativeMouseMode(grab, true);
}

void ActivateApp(uint32_t hwnd, bool active) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (active) {
      if (g_active == hwnd) return;  // already active: nothing to report
      g_active = g_focus = hwnd;
    } else {
      if (g_active != hwnd) return;
      g_active = g_focus = 0;
    }
  }
  WatchdogAppActive(active);
  Post(hwnd, WM_ACTIVATEAPP, active ? 1 : 0, 0);
  Post(hwnd, WM_ACTIVATE, active ? 1 : 0, 0);
  Post(hwnd, active ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}

// The window that gets mouse input over top-level window h at the current
// pointer: the capture window, else the deepest visible child under the
// pointer. 0 when that window is disabled (its input is dropped).
uint32_t MouseTarget(uint32_t h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t t = g_capture && Win(g_capture) ? g_capture : ChildAtScreen(h, g_mouse_x, g_mouse_y);
  Window* w = Win(t);
  return w && (w->enabled || t == g_capture) ? t : 0;
}

// The window that gets keyboard input in top-level window h: the focus
// window when it is h or one of its children.
uint32_t KeyTarget(uint32_t h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_focus && Win(g_focus) && TopLevel(g_focus) == h ? g_focus : h;
}

// Posts a mouse message to the window under the pointer (in its client
// coordinates).
void PostMouse(uint32_t h, uint32_t msg, uint32_t keys) {
  const uint32_t t = MouseTarget(h);
  if (!t) return;
  int ox, oy;
  WindowOrigin(t, &ox, &oy);
  Post(t, msg, keys, MkLparam(g_mouse_x - ox, g_mouse_y - oy));
}

void HandleSdlEvent(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_QUIT: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      for (auto& [h, w] : g_windows)
        if (w.sdl && !w.destroyed) Post(h, WM_CLOSE, 0, 0);
      break;
    }
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (uint32_t h = HwndForSdl(e.window.windowID)) Post(h, WM_CLOSE, 0, 0);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      AttachClip();
      if (uint32_t h = HwndForSdl(e.window.windowID)) ActivateApp(h, true);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      if (uint32_t h = HwndForSdl(e.window.windowID)) {
        // Release keys so nothing stays stuck while we're away.
        for (int vk = 0; vk < 256; vk++) g_key_down[vk] = 0;
        ActivateApp(h, false);
      }
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      uint32_t h = HwndForSdl(e.key.windowID);
      uint8_t vk = VkFromScancode(e.key.scancode);
      if (!h || !vk) break;
      bool down = e.type == SDL_EVENT_KEY_DOWN;
      bool was_down = g_key_down[GenericVk(vk)];
      SetKey(vk, down);
      SetKey(GenericVk(vk), down);
      uint8_t gvk = GenericVk(vk);
      bool ext;
      uint8_t sc = ScanCodeForVk(vk, &ext);
      if (e.key.scancode == SDL_SCANCODE_KP_ENTER) ext = true;
      if (!down || !was_down)  // DirectInput reports changes, not repeats
        EmitRawInput(RawInput::Key, uint8_t(sc | (ext ? 0x80 : 0)), down ? 1 : 0);
      uint32_t lp = 1u | (uint32_t(sc) << 16) | (ext ? 1u << 24 : 0);
      if (!down) lp |= 0xC0000000u;
      else if (was_down) lp |= 0x40000000u;
      bool alt = g_key_down[0x12];
      bool sys = alt || gvk == 0x79 /* F10 */ || gvk == 0x12;
      if (alt) lp |= 1u << 29;
      if (down) {
        uint32_t mods = (alt ? 1u : 0) | (g_key_down[0x11] ? 2u : 0) |
                        (g_key_down[0x10] ? 4u : 0);  // MOD_ALT, CONTROL, SHIFT
        const HotKey* hk = nullptr;
        {
          std::lock_guard<std::recursive_mutex> l(g_mu);
          for (const HotKey& k : g_hotkeys)
            if (k.vk == gvk && (k.mods & 7) == mods) hk = &k;
          if (hk) {
            HotKey k = *hk;
            Msg m{k.hwnd, 0x0312 /* WM_HOTKEY */, k.id, mods | gvk << 16, TickCount(),
                  g_mouse_x, g_mouse_y};
            PostTo(k.tid, m);
            break;
          }
        }
      }
      uint32_t msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN)
                          : (sys ? WM_SYSKEYUP : WM_KEYUP);
      Post(KeyTarget(h), msg, gvk, lp);
      break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      uint32_t h = HwndForSdl(e.motion.windowID);
      if (!h) break;
      int ox, oy;
      WindowOrigin(h, &ox, &oy);
      const ClientMap cm = MapToClient(h);
      int mw, mh;
      if (!Background() || AbsolutePointer(e.motion.which)) {
        int rx, ry;
        cm.ToClient(e.motion.x, e.motion.y, &rx, &ry);
        g_real_x = rx;
        g_real_y = ry;
        g_pointer_inside = true;
      }
      {
        static float rx = 0, ry = 0;  // sub-mickey remainder
        rx += e.motion.xrel;
        ry += e.motion.yrel;
        int dx = int(rx), dy = int(ry);
        rx -= dx;
        ry -= dy;
        if (dx) EmitRawInput(RawInput::MouseX, 0, dx);
        if (dy) EmitRawInput(RawInput::MouseY, 0, dy);
      }
      AttachClip();
      bool clipped = g_clip.on && g_clip.hwnd == h;
      // A game that reads absolute positions and never re-centres (2D
      // games in a display mode) gets the host pointer mapped onto its
      // screen; the virtual cursor is for games that re-centre it.
      const bool recentring = int64_t(TickCount()) - g_last_recentre_ms <= 250;
      if (clipped || (recentring && ExclusiveSize(h, &mw, &mh))) {
        // Virtual cursor: the game recentres it with SetCursorPos every
        // frame and steers its own pointer by the difference, as on a
        // fullscreen Windows desktop. Real motion moves the virtual
        // position; the host pointer is never warped (while clipped it
        // is grabbed in SDL's relative mode instead). Motion is not
        // scaled by how the frame is shown: one unit of host motion is
        // one pixel of the game's screen, as a mickey is on Windows, so
        // mouse look turns the same windowed, fullscreen and in any mode.
        static float fx = 0, fy = 0;  // sub-pixel remainder
        fx += e.motion.xrel;
        fy += e.motion.yrel;
        int dx = int(fx), dy = int(fy);
        fx -= dx;
        fy -= dy;
        if (AbsolutePointer(e.motion.which)) {
          // A scripted or touch position is absolute: the virtual cursor
          // goes there (it cannot be steered by deltas without touching
          // the screen's edges).
          int cx, cy;
          cm.ToClient(e.motion.x, e.motion.y, &cx, &cy);
          dx = ox + cx - g_mouse_x;
          dy = oy + cy - g_mouse_y;
        }
        if (clipped) {
          g_mouse_x = std::clamp(g_mouse_x + dx, g_clip.l, std::max(g_clip.l, g_clip.r - 1));
          g_mouse_y = std::clamp(g_mouse_y + dy, g_clip.t, std::max(g_clip.t, g_clip.b - 1));
        } else {
          g_mouse_x = std::clamp(g_mouse_x + dx, ox, ox + mw - 1);
          g_mouse_y = std::clamp(g_mouse_y + dy, oy, oy + mh - 1);
        }
      } else {
        int x, y;
        cm.ToClient(e.motion.x, e.motion.y, &x, &y);
        if (cm.fitted) {
          // Over the black bars: the pointer is at the screen's edge.
          x = std::clamp(x, 0, std::max(cm.cw - 1, 0));
          y = std::clamp(y, 0, std::max(cm.ch - 1, 0));
        }
        g_mouse_x = ox + x;
        g_mouse_y = oy + y;
      }
      PostMouse(h, WM_MOUSEMOVE, MouseKeys());
      break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      g_pointer_inside = false;
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      uint32_t h = HwndForSdl(e.button.windowID);
      if (!h) break;
      bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
      int ox, oy;
      WindowOrigin(h, &ox, &oy);
      int x = g_mouse_x - ox, y = g_mouse_y - oy;  // where motion left it
      if (e.button.button >= SDL_BUTTON_LEFT && e.button.button <= SDL_BUTTON_X2) {
        // SDL: left, middle, right, X1, X2; DirectInput: left, right, middle.
        static const uint8_t kDiButton[] = {0, 2, 1, 3, 4};
        EmitRawInput(RawInput::MouseButton, kDiButton[e.button.button - 1], down ? 1 : 0);
      }
      uint32_t base;
      uint8_t vk;
      switch (e.button.button) {
        case SDL_BUTTON_LEFT: base = WM_LBUTTONDOWN; vk = 0x01; break;
        case SDL_BUTTON_RIGHT: base = WM_RBUTTONDOWN; vk = 0x02; break;
        case SDL_BUTTON_MIDDLE: base = WM_MBUTTONDOWN; vk = 0x04; break;
        default: return;
      }
      SetKey(vk, down);
      (void)x;
      (void)y;
      bool dbl = false;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(MouseTarget(h));
        dbl = down && w && (w->class_style & CS_DBLCLKS) && e.button.clicks == 2;
      }
      uint32_t msg = !down ? base + 1 : dbl ? base + 2 : base;
      PostMouse(h, msg, MouseKeys());
      // The click a sticky on-screen key was held for.
      if (!down && vk == 0x01) ScreenButtonsClicked();
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      uint32_t h = HwndForSdl(e.wheel.windowID);
      if (!h) break;
      int delta = int(e.wheel.y * 120.0f);
      if (!delta) break;
      EmitRawInput(RawInput::Wheel, 0, delta);
      Post(KeyTarget(h), WM_MOUSEWHEEL,
           MouseKeys() | (uint32_t(uint16_t(int16_t(delta))) << 16),
           MkLparam(g_mouse_x, g_mouse_y));
      break;
    }
    default:
      break;
  }
}

// Writes a guest MSG.
void PutMsg(uint32_t va, const Msg& m) {
  Wr32(va + 0, m.hwnd);
  Wr32(va + 4, m.msg);
  Wr32(va + 8, m.wparam);
  Wr32(va + 12, m.lparam);
  Wr32(va + 16, m.time);
  Wr32(va + 20, uint32_t(m.ptx));
  Wr32(va + 24, uint32_t(m.pty));
}

bool Matches(const Msg& m, uint32_t hwnd, uint32_t lo, uint32_t hi) {
  if (hwnd && m.hwnd != hwnd) return false;
  if (lo == 0 && hi == 0) return true;
  return m.msg >= lo && m.msg <= hi;
}

// Takes (or peeks) the first matching message for the current thread.
bool TakeMessage(uint32_t hwnd, uint32_t lo, uint32_t hi, bool remove, Msg* out) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Queue& q = g_queues[CurrentThread()->tid];
  for (auto it = q.msgs.begin(); it != q.msgs.end(); ++it)
    if (Matches(*it, hwnd, lo, hi)) {
      *out = *it;
      if (remove) q.msgs.erase(it);
      return true;
    }
  if (q.quit) {
    *out = Msg{0, WM_QUIT, q.quit_code, 0, TickCount(), g_mouse_x, g_mouse_y};
    if (remove) q.quit = false;
    return true;
  }
  // WM_PAINT is never queued: it is generated while a window of this
  // thread has an update region (BeginPaint or ValidateRect empties it).
  // Parents come before their children (they were created first).
  for (auto& [h, w] : g_windows) {
    if (w.destroyed || !w.dirty || w.tid != CurrentThread()->tid || !ShownOnScreen(h)) continue;
    Msg m{h, WM_PAINT, 0, 0, TickCount(), g_mouse_x, g_mouse_y};
    if (!Matches(m, hwnd, lo, hi)) continue;
    *out = m;
    return true;
  }
  const uint32_t now = TickCount();
  for (Timer& t : g_timers) {
    Msg m{t.hwnd, WM_TIMER, t.id, t.proc, now, g_mouse_x, g_mouse_y};
    if (t.tid != CurrentThread()->tid || int64_t(now) < t.due || !Matches(m, hwnd, lo, hi))
      continue;
    *out = m;
    if (remove) t.due = int64_t(now) + t.elapse;
    return true;
  }
  return false;
}


GameConfig::Fullscreen FullscreenPolicy() {
  using F = GameConfig::Fullscreen;
  static const F f = [] {
    const char* e = Env("FULLSCREEN");
    if (!e || !*e) return Background() ? F::Window : Game().fullscreen;
    if (!std::strcmp(e, "window")) return F::Window;
    if (!std::strcmp(e, "desktop")) return F::Desktop;
    if (!std::strcmp(e, "exclusive")) return F::Exclusive;
    FatalF("FULLSCREEN=%s: expected window, desktop or exclusive", e);
  }();
  return f;
}

// Game sizes (screens, modes, client areas) are pixels; SDL sizes host
// windows in points, which differ on a scaled desktop. Windows are
// created with high pixel density, so a client of w x h pixels is a host
// window of w x h pixels at w / d x h / d points.
int ToPoints(SDL_Window* sdl, int px) {
  float d = sdl ? SDL_GetWindowPixelDensity(sdl) : g_desktop_density;
  if (!(d > 0)) d = 1;
  return std::max(int(std::lround(float(px) / d)), 1);
}

// The host window of a game window in points. One that covers the screen
// the game sees, under the window policy (GameConfig::fullscreen), is as
// large as fits 85% of the desktop's usable area at its own aspect (never
// smaller than its pixels in points: a 1180x768 game is not a 590x384 point
// window on a Retina screen); present keeps the aspect at any size the
// player then gives it (the window is resizable).
void SetHostSize(SDL_Window* sdl, int pw, int ph, bool covers) {
  int w = ToPoints(sdl, pw), h = ToPoints(sdl, ph);
  SDL_Rect r;
  if (covers && SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(sdl), &r) && r.w > 0 &&
      r.h > 0) {
    const double k = std::min(0.85 * r.w / pw, 0.85 * r.h / ph);
    if (k * pw > w) {
      w = int(std::lround(k * pw));
      h = int(std::lround(k * ph));
    }
  }
  SDL_SetWindowSize(sdl, w, h);
}

// The screen a window would cover: the display mode in exclusive mode,
// else the screen the game sees.
void ScreenFor(const Window& w, int* sw, int* sh) {
  *sw = w.mode_w ? w.mode_w : g_display_w;
  *sh = w.mode_w ? w.mode_h : g_display_h;
}

// Whether a visible top-level window covers the whole screen the game
// sees: DirectDraw exclusive mode, or a client area spanning the screen
// (a fullscreen window after a display mode change, or a borderless one at
// the desktop's size). Caller holds g_mu.
bool CoversScreen(const Window& w) {
  if (w.destroyed || !w.sdl || !w.visible) return false;
  if (w.mode_w) return true;
  int ox, oy, cw, ch;
  WindowOrigin(w.hwnd, &ox, &oy);
  ClientSize(w, &cw, &ch);
  return ox <= 0 && oy <= 0 && ox + cw >= g_display_w && oy + ch >= g_display_h;
}

// Host fullscreen follows CoversScreen (GameConfig::fullscreen). Called
// after anything that changes a window's placement, visibility or mode,
// or the screen size. Leaving it gives the host window the client size
// again.
void UpdateFullscreen(uint32_t hwnd) {
  using F = GameConfig::Fullscreen;
  SDL_Window* sdl;
  bool want;
  int sw, sh, cw, ch;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w || !w->sdl) return;
    want = FullscreenPolicy() != F::Window && CoversScreen(*w);
    ScreenFor(*w, &sw, &sh);
    if (want == w->fullscreen && (!want || (w->fs_w == sw && w->fs_h == sh))) return;
    w->fullscreen = want;
    w->fs_w = sw;
    w->fs_h = sh;
    sdl = w->sdl;
    GameClientSize(*w, &cw, &ch);
  }
  if (want) {
    // Exclusive: the smallest host mode that holds the game's screen at
    // the display's own aspect (a 4:3 mode on a 16:9 display asks for
    // 16:9 at the same height), so the monitor or compositor never
    // stretches it and present adds the bars. None found, or desktop
    // policy: the desktop's mode.
    // Sizes compared in pixels (a mode's w x h times its pixel density).
    SDL_DisplayMode m;
    bool exclusive = false;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(sdl);
    const SDL_DisplayMode* d = SDL_GetDesktopDisplayMode(display);
    if (FullscreenPolicy() == F::Exclusive && d && d->w > 0 && d->h > 0) {
      int tw = sw, th = sh;
      if (int64_t(sw) * d->h < int64_t(sh) * d->w)
        tw = int((int64_t(sh) * d->w + d->h - 1) / d->h);
      else
        th = int((int64_t(sw) * d->h + d->w - 1) / d->w);
      int n = 0;
      SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &n);
      int64_t best = 0;
      for (int i = 0; i < n; i++) {
        const float k = modes[i]->pixel_density > 0 ? modes[i]->pixel_density : 1.0f;
        const int64_t pw = std::lround(float(modes[i]->w) * k);
        const int64_t ph = std::lround(float(modes[i]->h) * k);
        if (pw < tw || ph < th || (exclusive && pw * ph >= best)) continue;
        m = *modes[i];
        best = pw * ph;
        exclusive = true;
      }
      SDL_free(modes);
    }
    SDL_SetWindowFullscreenMode(sdl, exclusive ? &m : nullptr);
    SDL_SetWindowFullscreen(sdl, true);
    if (exclusive)
      std::fprintf(stderr, "user32: window %X fullscreen for a %dx%d screen, host mode %dx%d\n",
                   hwnd, sw, sh, m.w, m.h);
    else
      std::fprintf(stderr, "user32: window %X fullscreen for a %dx%d screen\n", hwnd, sw, sh);
  } else {
    SDL_SetWindowFullscreen(sdl, false);
    SDL_SetWindowSize(sdl, ToPoints(sdl, gfx::ShownWidth(cw)), ToPoints(sdl, ch));
  }
}

void UpdateFullscreenAll() {
  std::vector<uint32_t> hwnds;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (w.sdl && !w.destroyed) hwnds.push_back(h);
  }
  for (uint32_t h : hwnds) UpdateFullscreen(h);
}

}  // namespace user32

using namespace user32;

bool OnMainThread() { return std::this_thread::get_id() == g_main_thread_id; }

namespace {
void StickEdge(int dx, int dy);  // below, with the touch handling
void ScreenButtonAction(size_t i, ButtonEvent ev);
}  // namespace

void EnsureSdlVideo() {
  if (g_sdl_up) return;
  if (!OnMainThread()) FatalF("SDL video requested off the main thread");
  if (Background()) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
  // Leave SIGINT/SIGTERM alone: SDL would turn them into quit events the
  // game may ignore, making the process hard to stop.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  // <ID>_BARS=white|RRGGBB: the colour of the bars around a fitted frame
  // (where the game's picture ends is then plain to see).
  if (const char* bars = Env("BARS")) {
    const unsigned long v = std::string(bars) == "white" ? 0xFFFFFF : std::strtoul(bars, nullptr, 16);
    gfx::SetBarColor(float((v >> 16) & 0xFF) / 255, float((v >> 8) & 0xFF) / 255,
                     float(v & 0xFF) / 255);
  }
  SetStickEdgeHandler(StickEdge);
  SetScreenButtonHandler(ScreenButtonAction);
  // Touch gestures make their own mouse events (HandleTouch).
  if (Game().touch == GameConfig::Touch::Pointer) SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  if (!SDL_Init(SDL_INIT_VIDEO)) FatalF("SDL_Init: %s", SDL_GetError());
  g_sdl_up = true;
  if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
    // Games see pixels: a scaled desktop (Wayland, macOS) reports points.
    const float d = m->pixel_density > 0 ? m->pixel_density : 1.0f;
    g_desktop_density = d;
    g_display_w = g_desktop_w = int(std::lround(float(m->w) * d));
    g_display_h = g_desktop_h = int(std::lround(float(m->h) * d));
  }
}

void DesktopSize(int* w, int* h) {
  if (OnMainThread()) EnsureSdlVideo();
  *w = g_desktop_w;
  *h = g_desktop_h;
}

namespace {

// Test hook: <ID>_TEST_FOCUS="lost,gained" (seconds since the first pump)
// injects window focus loss and regain, to exercise the game's
// deactivation path without touching the desktop.
void InjectTestFocus() {
  static int stage = -1;
  static double lost = 0, gained = 0;
  static Uint64 t0 = 0;
  if (stage == -1) {
    stage = 2;
    const char* env = Env("TEST_FOCUS");
    if (env && std::sscanf(env, "%lf,%lf", &lost, &gained) == 2) stage = 0;
    t0 = SDL_GetTicks();
  }
  if (stage >= 2) return;
  double t = (SDL_GetTicks() - t0) / 1000.0;
  if (t < (stage == 0 ? lost : gained)) return;
  uint32_t hwnd = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (w.sdl && !w.destroyed) hwnd = h;
  }
  if (!hwnd) return;
  WarnF("TEST_FOCUS: focus %s at %.1f s\n", stage == 0 ? "lost" : "gained", t);
  if (stage == 0) {
    for (int vk = 0; vk < 256; vk++) g_key_down[vk] = 0;
    ActivateApp(hwnd, false);
  } else {
    ActivateApp(hwnd, true);
  }
  stage++;
}

// Test hook: <ID>_TEST_INPUT=<file> replays scripted input, one command
// per line, times in seconds since the first event pump:
//   <t> goto <x> <y>      move the game's cursor to client (x, y): it is
//                         driven by relative motion, so this homes it to
//                         the top-left corner first, over a few frames
//   <t> move <dx> <dy>    relative motion
//   <t> pointer <x> <y>   the host pointer is at client (x, y) (the
//                         game's cursor should follow it exactly; a
//                         virtual cursor is put there)
//   <t> click [left|right]
//   <t> touch down|move|up <finger> <x> <y>   a finger on a touch screen
//                         at client (x, y), through the touch gestures
//   <t> key <SDL scancode name> [seconds]   e.g. Escape, Return, Space, A;
//                         held for that long (default 0.08)
// '#' starts a comment.
bool IsFingerEvent(uint32_t type);
bool HandleTouch(const SDL_Event& e);

struct TestEvent {
  double t;
  SDL_Event e;
};

// Appends the events of one script line, its time offset by `base`
// seconds; false for a line that is not a command.
bool ParseTestLine(char* line, double base, std::vector<TestEvent>& q) {
  auto motion = [](double t, float dx, float dy) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.xrel = dx;
    e.motion.yrel = dy;
    return TestEvent{t, e};
  };
  if (char* h = std::strchr(line, '#')) *h = 0;
  double t;
  char cmd[32] = {}, a[64] = {}, b[64] = {};
  int n = std::sscanf(line, "%lf %31s %63s %63s", &t, cmd, a, b);
  if (n < 2) return true;
  t += base;
  std::string c = cmd;
  if (c == "goto" && n == 4) {
    // Home: two big moves (the virtual cursor clamps at the edge and
    // the game re-centres it every frame), then steps of <= 200 px.
    q.push_back(motion(t, -4000, -4000));
    q.push_back(motion(t + 0.1, -4000, -4000));
    q.push_back(motion(t + 0.2, -4000, -4000));
    float x = std::atof(a), y = std::atof(b);
    double tt = t + 0.3;
    while (x > 0 || y > 0) {
      float sx = std::min(x, 200.0f), sy = std::min(y, 200.0f);
      q.push_back(motion(tt, sx, sy));
      x -= sx;
      y -= sy;
      tt += 0.1;
    }
  } else if (c == "pointer" && n == 4) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.which = kTestPointer;
    e.motion.x = std::atof(a);
    e.motion.y = std::atof(b);
    q.push_back({t, e});
  } else if (c == "move" && n == 4) {
    q.push_back(motion(t, std::atof(a), std::atof(b)));
  } else if (c == "touch") {
    // <t> touch down|move|up <finger> <x> <y>: a finger on a direct touch
    // screen at client (x, y).
    int finger = 0;
    float fx = 0, fy = 0;
    char phase[16] = {};
    if (std::sscanf(line, "%*lf %*s %15s %d %f %f", phase, &finger, &fx, &fy) != 4) return false;
    const std::string ph = phase;
    SDL_Event e{};
    e.type = ph == "down" ? SDL_EVENT_FINGER_DOWN
             : ph == "move" ? SDL_EVENT_FINGER_MOTION
             : ph == "up"   ? SDL_EVENT_FINGER_UP
                            : 0;
    if (!e.type) return false;
    e.tfinger.touchID = kTestTouch;
    e.tfinger.fingerID = SDL_FingerID(finger);
    e.tfinger.x = fx;
    e.tfinger.y = fy;
    q.push_back({t, e});
  } else if (c == "click") {
    bool right = n >= 3 && std::string(a) == "right";
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
    e.button.down = true;
    e.button.clicks = 1;
    q.push_back({t, e});
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.down = false;
    q.push_back({t + 0.08, e});
  } else if (c == "key" && n >= 3) {
    SDL_Scancode sc = SDL_GetScancodeFromName(a);
    if (sc == SDL_SCANCODE_UNKNOWN) {
      WarnF("TEST_INPUT: unknown key %s\n", a);
      return true;
    }
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = sc;
    e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    e.key.down = true;
    q.push_back({t, e});
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    q.push_back({t + (n >= 4 ? std::atof(b) : 0.08), e});
  } else {
    return false;
  }
  return true;
}

// With <ID>_TEST_INPUT_LIVE=1 the script stays open: lines appended while
// the game runs are read as they arrive, their times counted from then
// (for driving a run step by step from outside).
void InjectTestInput() {
  static std::vector<TestEvent> q;
  static size_t next = 0;
  static Uint64 t0 = 0;
  static bool loaded = false;
  static FILE* live = nullptr;
  char line[256];
  if (!loaded) {
    loaded = true;
    t0 = SDL_GetTicks();
    const char* path = Env("TEST_INPUT");
    if (!path) return;
    FILE* f = std::fopen(path, "r");
    if (!f) {
      WarnF("TEST_INPUT: cannot open %s\n", path);
      return;
    }
    while (std::fgets(line, sizeof line, f))
      if (!ParseTestLine(line, 0, q)) WarnF("TEST_INPUT: bad line: %s", line);
    std::stable_sort(q.begin(), q.end(),
                     [](const TestEvent& x, const TestEvent& y) { return x.t < y.t; });
    WarnF("TEST_INPUT: %zu events from %s\n", q.size(), path);
    const char* lv = Env("TEST_INPUT_LIVE");
    if (lv && *lv && *lv != '0')
      live = f;
    else
      std::fclose(f);
  }
  if (live) {
    const double now = (SDL_GetTicks() - t0) / 1000.0;
    const size_t before = q.size();
    std::clearerr(live);
    while (std::fgets(line, sizeof line, live))
      if (!ParseTestLine(line, now, q)) WarnF("TEST_INPUT: bad line: %s", line);
    if (q.size() != before)
      std::stable_sort(q.begin() + next, q.end(),
                       [](const TestEvent& x, const TestEvent& y) { return x.t < y.t; });
  }
  if (next >= q.size()) return;
  double t = (SDL_GetTicks() - t0) / 1000.0;
  SDL_WindowID id = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (w.sdl && !w.destroyed) id = SDL_GetWindowID(w.sdl);
  }
  if (!id) return;
  // At most one motion per pump, so each lands in its own game frame.
  bool moved = false;
  while (next < q.size() && q[next].t <= t) {
    SDL_Event e = q[next].e;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
      if (moved) break;
      moved = true;
      e.motion.windowID = id;
    } else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
      e.key.windowID = id;
    } else if (IsFingerEvent(e.type)) {
      e.tfinger.windowID = id;
      if (!HandleTouch(e)) WarnF("TEST_INPUT: touch events need GameConfig::Touch::Pointer\n");
      next++;
      continue;
    } else {
      e.button.windowID = id;
    }
    HandleSdlEvent(e);
    next++;
  }
}

// Test hook: <ID>_TEST_STALL="at,seconds" blocks the main thread once, to
// exercise the hang watchdog.
void InjectTestStall() {
  static double at = -1, dur = 0;
  static bool done = false;
  static Uint64 t0 = SDL_GetTicks();
  if (done) return;
  if (at < 0) {
    const char* env = Env("TEST_STALL");
    if (!env || std::sscanf(env, "%lf,%lf", &at, &dur) != 2) {
      done = true;
      return;
    }
  }
  if ((SDL_GetTicks() - t0) / 1000.0 < at) return;
  done = true;
  WarnF("TEST_STALL: blocking the main thread for %.1f s\n", dur);
  std::this_thread::sleep_for(std::chrono::duration<double>(dur));
}

void (*g_host_event_sink)(const SDL_Event&) = nullptr;

}  // namespace

void SetHostEventSink(void (*sink)(const SDL_Event&)) { g_host_event_sink = sink; }

uint32_t CreateSdl2Window(const std::string& title_utf8, int w, int h, SDL_WindowFlags flags,
                          SDL_Window** sdl) {
  EnsureSdlVideo();
  Window win;
  win.sdl = SDL_CreateWindow(title_utf8.c_str(), ToPoints(nullptr, w), ToPoints(nullptr, h),
                             flags | SDL_WINDOW_HIGH_PIXEL_DENSITY | hle::GfxWindowFlags());
  if (!win.sdl) return 0;
  win.title = title_utf8;
  win.w = w;
  win.h = h;
  win.visible = !(flags & SDL_WINDOW_HIDDEN);
  win.tid = CurrentThread()->tid;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  win.hwnd = g_next_hwnd;
  g_next_hwnd += 0x10;
  *sdl = win.sdl;
  uint32_t hwnd = win.hwnd;
  g_windows[hwnd] = std::move(win);
  return hwnd;
}

void DestroySdl2Window(uint32_t hwnd) {
  SDL_Window* sdl = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    auto it = g_windows.find(hwnd);
    if (it == g_windows.end()) return;
    sdl = it->second.sdl;
    g_windows.erase(it);
  }
  if (sdl) SDL_DestroyWindow(sdl);
}

namespace {

void DeliverHostEvent(const SDL_Event& e) {
  if (g_host_event_sink)
    g_host_event_sink(e);
  else
    HandleSdlEvent(e);
}

}  // namespace

namespace {

TouchGestures g_touch;
SDL_WindowID g_touch_window = 0;
// The mouse's pointer id for the on-screen controls (fingers use theirs).
constexpr uint64_t kMouseControlId = ~uint64_t(0);

// The window the game is shown in: the one in exclusive mode, else the
// first visible top-level window with a host window.
SDL_Window* PrimaryHostWindow() {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  SDL_Window* first = nullptr;
  for (auto& [h, w] : g_windows) {
    if (!w.sdl || w.destroyed) continue;
    if (w.mode_w) return w.sdl;
    if (!first && w.visible) first = w.sdl;
  }
  return first;
}

// The on-screen stick (screen_controls.h): the pointer goes to the edge of
// the window in the stick's direction (the frame's edge where it is
// fitted with bars: the pointer is clamped there), or to the middle when
// it is let go.
void StickEdge(int dx, int dy) {
  SDL_Window* w = PrimaryHostWindow();
  if (!w) return;
  int ww = 0, wh = 0;
  SDL_GetWindowSize(w, &ww, &wh);
  SDL_Event m{};
  m.type = SDL_EVENT_MOUSE_MOTION;
  m.motion.which = kTouchPointer;
  m.motion.windowID = SDL_GetWindowID(w);
  m.motion.x = dx < 0 ? 0.0f : dx > 0 ? float(ww - 1) : float(ww) / 2;
  m.motion.y = dy < 0 ? 0.0f : dy > 0 ? float(wh - 1) : float(wh) / 2;
  HandleSdlEvent(m);
}

// The scancode of a key the on-screen buttons press (a letter, a digit or
// a modifier).
SDL_Scancode ScancodeForButtonVk(size_t i, uint8_t vk) {
  if (vk >= 'A' && vk <= 'Z') return SDL_Scancode(SDL_SCANCODE_A + (vk - 'A'));
  if (vk >= '1' && vk <= '9') return SDL_Scancode(SDL_SCANCODE_1 + (vk - '1'));
  switch (vk) {
    case '0': return SDL_SCANCODE_0;
    case 0x10: return SDL_SCANCODE_LSHIFT;
    case 0x11: return SDL_SCANCODE_LCTRL;
    case 0x12: return SDL_SCANCODE_LALT;
    default: FatalF("screen button %zu: key %02X is not a letter, digit or modifier", i, vk);
  }
}

void ButtonKey(SDL_WindowID wid, SDL_Scancode sc, bool down) {
  SDL_Event k{};
  k.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  k.key.windowID = wid;
  k.key.scancode = sc;
  k.key.down = down;
  HandleSdlEvent(k);
}

// The buttons around the stick (GameConfig::screen_buttons): a right click
// at the button's point of the game's screen, its key (or double tap key)
// pressed with its modifier held until the matching end event, or its
// sticky key held or let go.
void ScreenButtonAction(size_t i, ButtonEvent ev) {
  using Action = GameConfig::ScreenButton::Action;
  const auto& cfg = Game().screen_buttons;
  SDL_Window* w = PrimaryHostWindow();
  if (i >= cfg.size() || !w) return;
  const GameConfig::ScreenButton& b = cfg[i];
  const SDL_WindowID wid = SDL_GetWindowID(w);
  // <ID>_BUTTON_LOG=1: each button event on stderr.
  static const bool log = [] {
    const char* e = Env("BUTTON_LOG");
    return e && *e && *e != '0';
  }();
  if (log) {
    static const char* const kEv[] = {"tap", "tap end", "double tap", "double tap end", "arm",
                                      "disarm"};
    std::fprintf(stderr, "screen button %zu: %s (vk %02X, window %u, focus %08X)\n", i,
                 kEv[int(ev)], b.vk, unsigned(wid), g_focus);
  }
  if (b.action == Action::StickyKey) {
    ButtonKey(wid, ScancodeForButtonVk(i, b.vk), ev == ButtonEvent::Arm);
    return;
  }
  if (b.action == Action::Key) {
    const bool twice = ev == ButtonEvent::DoubleTap || ev == ButtonEvent::DoubleTapEnd;
    const uint8_t vk = twice ? b.double_vk : b.vk, mod = twice ? b.double_mod : b.mod;
    if (ev == ButtonEvent::TapEnd || ev == ButtonEvent::DoubleTapEnd) {
      if (mod) ButtonKey(wid, ScancodeForButtonVk(i, mod), false);
      return;
    }
    if (ev != ButtonEvent::Tap && ev != ButtonEvent::DoubleTap) return;
    // The modifier stays down until the end event: the game reads its state
    // when it handles the key, after this returns.
    const SDL_Scancode key = ScancodeForButtonVk(i, vk);
    if (mod) ButtonKey(wid, ScancodeForButtonVk(i, mod), true);
    ButtonKey(wid, key, true);
    ButtonKey(wid, key, false);
    return;
  }
  if (ev != ButtonEvent::Tap) return;
  const uint32_t h = HwndForSdl(wid);
  if (!h) return;
  const ClientMap cm = MapToClient(h);
  SDL_Event m{};
  m.type = SDL_EVENT_MOUSE_MOTION;
  m.motion.which = kTouchPointer;
  m.motion.windowID = wid;
  m.motion.x = cm.x0 + b.click_x * float(cm.cw) / cm.kx;
  m.motion.y = cm.y0 + b.click_y * float(cm.ch) / cm.ky;
  HandleSdlEvent(m);
  for (const bool down : {true, false}) {
    SDL_Event c{};
    c.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    c.button.which = kTouchPointer;
    c.button.windowID = wid;
    c.button.button = SDL_BUTTON_RIGHT;
    c.button.down = down;
    c.button.clicks = 1;
    c.button.x = m.motion.x;
    c.button.y = m.motion.y;
    HandleSdlEvent(c);
  }
}

// A mouse event the on-screen controls take (desktop testing of the stick).
bool MouseToControls(const SDL_Event& e) {
  if (Game().touch_stick == GameConfig::Stick::None) return false;
  SDL_Window* w = SDL_GetWindowFromID(e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.windowID
                                                                     : e.button.windowID);
  const float k = w ? SDL_GetWindowPixelDensity(w) : 1.0f;
  if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT &&
      e.button.which != SDL_TOUCH_MOUSEID)
    return ScreenControlsDown(kMouseControlId, e.button.x * k, e.button.y * k);
  if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which != SDL_TOUCH_MOUSEID)
    return ScreenControlsMove(kMouseControlId, e.motion.x * k, e.motion.y * k);
  if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT)
    return ScreenControlsUp(kMouseControlId);
  return false;
}

bool IsFingerEvent(uint32_t type) {
  return type == SDL_EVENT_FINGER_DOWN || type == SDL_EVENT_FINGER_MOTION ||
         type == SDL_EVENT_FINGER_UP || type == SDL_EVENT_FINGER_CANCELED;
}

// The gestures' mouse actions, as events from kTouchPointer.
void FlushTouch() {
  for (const TouchAction& a : g_touch.Take()) {
    SDL_Event m{};
    if (a.kind == TouchAction::Kind::Move) {
      m.type = SDL_EVENT_MOUSE_MOTION;
      m.motion.which = kTouchPointer;
      m.motion.windowID = g_touch_window;
      m.motion.x = a.x;
      m.motion.y = a.y;
    } else {
      const bool down = a.kind == TouchAction::Kind::Down;
      m.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
      m.button.which = kTouchPointer;
      m.button.windowID = g_touch_window;
      m.button.button = uint8_t(a.button);
      m.button.down = down;
      m.button.clicks = uint8_t(a.clicks);
      m.button.x = a.x;
      m.button.y = a.y;
    }
    HandleSdlEvent(m);
  }
}

// A finger on a direct touch screen, with GameConfig::Touch::Pointer:
// fed to the gestures. False for events this does not take.
bool HandleTouch(const SDL_Event& e) {
  if (Game().touch != GameConfig::Touch::Pointer || !IsFingerEvent(e.type)) return false;
  const SDL_TouchID t = e.tfinger.touchID;
  if (t != kTestTouch && SDL_GetTouchDeviceType(t) != SDL_TOUCH_DEVICE_DIRECT) return false;
  float x = e.tfinger.x, y = e.tfinger.y;
  SDL_Window* fw = SDL_GetWindowFromID(e.tfinger.windowID);
  if (t != kTestTouch) {  // normalised to the window
    int ww = 0, wh = 0;
    if (fw) SDL_GetWindowSize(fw, &ww, &wh);
    x *= float(ww);
    y *= float(wh);
  }
  // The on-screen controls first (they work in the drawable's pixels).
  {
    const float k = fw ? SDL_GetWindowPixelDensity(fw) : 1.0f;
    const uint64_t fid = e.tfinger.fingerID;
    const bool taken = e.type == SDL_EVENT_FINGER_DOWN     ? ScreenControlsDown(fid, x * k, y * k)
                       : e.type == SDL_EVENT_FINGER_MOTION ? ScreenControlsMove(fid, x * k, y * k)
                                                           : ScreenControlsUp(fid);
    if (taken) return true;
  }
  static const bool pan_set = [] {
    g_touch.SetPan(Game().touch_pan_offset, Game().touch_pan_speed);
    return true;
  }();
  (void)pan_set;
  g_touch_window = e.tfinger.windowID;
  const uint64_t ms = SDL_GetTicks(), id = e.tfinger.fingerID;
  if (e.type == SDL_EVENT_FINGER_DOWN)
    g_touch.Down(id, x, y, ms);
  else if (e.type == SDL_EVENT_FINGER_MOTION)
    g_touch.Move(id, x, y, ms);
  else
    g_touch.Up(id, x, y, ms);
  FlushTouch();
  return true;
}

}  // namespace

void (*g_frame_end)(bool again) = nullptr;
// SDL ticks of the last GameFrameEnd; pumps leave the frame end to it for
// kGamePacedMs after (when the game stops reporting frames, e.g. in its
// menus, pumps show frames again).
Uint64 g_game_frame_ms = 0;
constexpr Uint64 kGamePacedMs = 100;

// The steady present clock (GameConfig::present_rate): when its next
// slot comes, in ns of SDL_GetTicksNS (0: not started).
Uint64 g_next_slot_ns = 0;

void SetFrameEndHandler(void (*fn)(bool again)) { g_frame_end = fn; }

void GameFrameEnd(bool show) {
  if (!OnMainThread()) return;
  g_game_frame_ms = SDL_GetTicks();
  if (show && g_frame_end) g_frame_end(true);
}

namespace {

bool GamePaced() { return g_game_frame_ms && SDL_GetTicks() - g_game_frame_ms < kGamePacedMs; }

uint64_t SlotNs() {
  const int rate = Game().present_rate;
  return rate > 0 ? 1'000'000'000ull / uint64_t(rate) : 0;
}

// Whether the game's window can be seen: showing the same picture again
// in a hidden, minimised or covered window only waits for the window
// system to hand out a drawable.
bool GameWindowVisible() {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows) {
    if (w.destroyed || !w.sdl || !w.mode_w) continue;
    const SDL_WindowFlags f = SDL_GetWindowFlags(w.sdl);
    return !(f & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED));
  }
  return false;
}

// At a pump or a main-thread Sleep, outside game-paced frames: without a
// present rate, shows what was drawn since the last frame end; with one,
// shows the picture when a slot of the present clock has come (again if
// nothing new was drawn and the window can be seen), so frames are shown
// at an even rate whenever the game draws.
void FrameEndOutsideGameFrames() {
  if (!g_frame_end || GamePaced()) return;
  const uint64_t slot = SlotNs();
  if (!slot) {
    g_frame_end(false);
    return;
  }
  const Uint64 now = SDL_GetTicksNS();
  if (g_next_slot_ns && now < g_next_slot_ns) return;
  // On the grid of slots; after a gap (loading, a pause), from now.
  g_next_slot_ns = g_next_slot_ns && now - g_next_slot_ns < slot ? g_next_slot_ns + slot : now + slot;
  g_frame_end(GameWindowVisible());
}

}  // namespace

void SleepShowingFrames(uint32_t ms) {
  const Uint64 end = SDL_GetTicksNS() + uint64_t(ms) * 1'000'000ull;
  if (g_sdl_up && OnMainThread() && SlotNs()) {
    for (;;) {
      FrameEndOutsideGameFrames();
      const Uint64 now = SDL_GetTicksNS();
      if (!g_next_slot_ns || g_next_slot_ns >= end || GamePaced()) break;
      if (g_next_slot_ns > now)
        std::this_thread::sleep_for(std::chrono::nanoseconds(g_next_slot_ns - now));
    }
  }
  const Uint64 now = SDL_GetTicksNS();
  if (end > now) std::this_thread::sleep_for(std::chrono::nanoseconds(end - now));
}

void PumpHostEvents() {
  if (!g_sdl_up || !OnMainThread()) return;
  FrameEndOutsideGameFrames();
  InjectTestStall();
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    // Automated runs take input only from <ID>_TEST_INPUT: their windows
    // are on someone's desktop and stray clicks must not drive them.
    if (Background() && (e.type == SDL_EVENT_MOUSE_MOTION ||
                         e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                         e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                         e.type == SDL_EVENT_MOUSE_WHEEL ||
                         e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP ||
                         e.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
                         e.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                         e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE || IsFingerEvent(e.type)))
      continue;
    if (HandleTouch(e)) continue;
    if (MouseToControls(e)) continue;
    // With gestures, SDL's own mouse emulation of touches is off; drop
    // any that still come.
    if (Game().touch == GameConfig::Touch::Pointer &&
        ((e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which == SDL_TOUCH_MOUSEID) ||
         ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
          e.button.which == SDL_TOUCH_MOUSEID)))
      continue;
    DeliverHostEvent(e);
  }
  g_touch.Tick(SDL_GetTicks());
  FlushTouch();
  ScreenControlsTick(PrimaryHostWindow());
  InjectTestFocus();
  InjectTestInput();
  WatchdogHeartbeat();
}

uint32_t DcForWindow(uint32_t hwnd) { return hwnd ? 0x40000000u | hwnd : 0x00D00001u; }

uint32_t WindowForDc(uint32_t hdc) { return hdc & 0x40000000u ? hdc & ~0x40000000u : 0; }

SDL_Window* SdlWindowFor(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  return w ? w->sdl : nullptr;
}

void SetExclusiveMode(uint32_t hwnd, int w, int h) {
  SDL_Window* sdl;
  bool covers = false;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* win = Win(hwnd);
    if (!win) return;
    win->mode_w = w;
    win->mode_h = h;
    sdl = win->fullscreen ? nullptr : win->sdl;
    covers = FullscreenPolicy() == GameConfig::Fullscreen::Window && CoversScreen(*win);
  }
  // Wider by the side panel's growth, when there is one (gfx::ShownWidth).
  if (sdl && w) SetHostSize(sdl, gfx::ShownWidth(w), h, covers);
  UpdateFullscreen(hwnd);
}

void ShowWindowNoActivate(uint32_t hwnd) {
  SDL_Window* sdl;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* win = Win(hwnd);
    if (!win || win->visible) return;
    win->visible = true;
    sdl = win->sdl;
  }
  if (sdl) SDL_ShowWindow(sdl);
  UpdateFullscreen(hwnd);
}

std::vector<ModeSize> DisplayModeSizes() {
  static const ModeSize kSizes[] = {
      {640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 960},
      {1280, 1024}, {1600, 1200}, {1280, 720},  {1366, 768},  {1600, 900},
      {1920, 1080}, {1920, 1200}, {2560, 1080}, {2560, 1440}, {3440, 1440},
      {3840, 2160}};
  EnsureSdlVideo();
  // Never smaller than a 1024x768 desktop, so the classic modes stay.
  const int mw = std::max(g_desktop_w, 1024), mh = std::max(g_desktop_h, 768);
  std::vector<ModeSize> v{{g_desktop_w, g_desktop_h}};
  for (const ModeSize& s : kSizes)
    if (s.w <= mw && s.h <= mh) v.push_back(s);
  // The desktop's own shape at the classic heights (widths a multiple of
  // 8), so a game can fill a screen of any aspect (a phone's) without
  // drawing at the full native size.
  const int dw = std::max(g_desktop_w, g_desktop_h), dh = std::min(g_desktop_w, g_desktop_h);
  if (dw > 0 && dh > 0)
    for (int h : {600, 720, 768}) {
      const int w = int(std::lround(double(h) * dw / dh / 8.0)) * 8;
      if (h < dh && w < dw) v.push_back({w, h});
    }
  std::sort(v.begin(), v.end(),
            [](const ModeSize& a, const ModeSize& b) { return a.w != b.w ? a.w < b.w : a.h < b.h; });
  v.erase(std::unique(v.begin(), v.end(),
                      [](const ModeSize& a, const ModeSize& b) { return a.w == b.w && a.h == b.h; }),
          v.end());
  return v;
}

void ClientAreaSize(uint32_t hwnd, int* w, int* h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  *w = *h = 0;
  if (Window* win = Win(hwnd)) GameClientSize(*win, w, h);
}

namespace hle {
namespace {

using namespace win32;

// ---- classes ---------------------------------------------------------------

// g_classes key of a class name argument: an atom (MAKEINTATOM) or a name.
std::string ClassKey(uint32_t name) {
  return name < 0x10000 ? "#" + std::to_string(name) : Lower(GStr(name));
}

// The system control classes every process has (see "built-in control
// classes" below). Their window procedures are host handlers behind
// thunks, so games can subclass them and chain with CallWindowProc.
}  // namespace
}  // namespace hle

uint32_t user32::ClassProc(const std::string& thunk_name) {
  return hle::ThunkFor("user32.dll", thunk_name);
}

void user32::EnsureSystemClasses() {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (g_classes.count("edit")) return;
  // name, procedure thunk, class style. "#32770" is the dialog class
  // (WC_DIALOG); the comctl32 classes are registered with the others.
  // The window extra bytes are laid out as the system classes' are, for
  // games that read them (GetWindowLong with an offset): a button keeps
  // its state, font and image at 0, 4 and 8; a static control its font and
  // icon at 0 and 4; edit, list and combo boxes a pointer to their state;
  // a dialog the DWL_ values (DLGWINDOWEXTRA, 30 bytes).
  // Names as Windows spells them: GetClassName returns them so, and games
  // compare them case-sensitively ("Button", "ComboBox").
  struct Sys {
    const char* name;
    const char* proc;
    uint32_t style, extra;
  };
  static const Sys kSys[] = {
      {"Static", "<static>", 0, 8},
      {"Edit", "<edit>", CS_DBLCLKS, 4},
      {"Button", "<button>", CS_DBLCLKS, 12},
      {"ListBox", "<listbox>", CS_DBLCLKS, 4},
      {"ComboBox", "<combobox>", CS_DBLCLKS, 4},
      {"ComboLBox", "<combolbox>", 0, 4},
      {"ScrollBar", "<scrollbar>", CS_DBLCLKS, 0},
      {"#32770", "<dialog>", 0, 30},
      {"msctls_trackbar32", "<trackbar>", 0, 4},
      {"msctls_progress32", "<progress>", 0, 4},
  };
  for (const Sys& s : kSys) {
    WndClass k;
    k.name = hle::Lower(s.name);
    k.display = s.name;
    k.wndproc = ClassProc(s.proc);
    k.style = s.style;
    k.wnd_extra = s.extra;
    k.atom = g_next_atom++;
    g_classes[k.name] = k;
  }
}

namespace hle {
namespace {

// WNDCLASSA at wc, or the same fields 4 bytes on in WNDCLASSEXA (after
// cbSize).
uint32_t RegisterWndClass(uint32_t wc) {
  WndClass k;
  k.style = Rd32(wc + 0);
  k.wndproc = Rd32(wc + 4);
  k.wnd_extra = Rd32(wc + 12);
  k.name = ClassKey(Rd32(wc + 36));
  k.display = Rd32(wc + 36) < 0x10000 ? k.name : GStr(Rd32(wc + 36));
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (g_classes.count(k.name)) {
    SetLastError(kErrorClassAlreadyExists);
    return 0;
  }
  k.atom = g_next_atom++;
  g_classes[k.name] = k;
  return k.atom;
}

HLE("user32.dll", RegisterClassA, 1) { return RegisterWndClass(Arg(c, 0)); }

HLE("user32.dll", RegisterClassExA, 1) {
  uint32_t wc = Arg(c, 0);
  if (Rd32(wc) != 48) {  // cbSize: sizeof(WNDCLASSEXA)
    SetLastError(87);    // ERROR_INVALID_PARAMETER
    return 0;
  }
  return RegisterWndClass(wc + 4);
}

HLE("user32.dll", UnregisterClassA, 2) {
  std::string n = ClassKey(Arg(c, 0));
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_classes.erase(n) ? 1 : 0;
}

HLE("user32.dll", SetClassLongA, 3) {
  // GCL_HCURSOR (-12), GCL_HICON (-14)... cosmetic; remember nothing.
  return 0;
}

// ---- windows ---------------------------------------------------------------

int64_t g_z_bottom = 0;  // the next child's place: below every sibling

// CreateWindowExA with the class and title as guest strings (cls_va may
// be an atom).
uint32_t CreateWindowCore(uint32_t exstyle, uint32_t cls_va, uint32_t title_va, uint32_t style,
                          int32_t x, int32_t y, int32_t w, int32_t h, uint32_t parent,
                          uint32_t menu, uint32_t hinst, uint32_t param) {
  std::string cls = ClassKey(cls_va);
  EnsureSystemClasses();
  Window win;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    auto it = g_classes.find(cls);
    if (it == g_classes.end()) {
      WarnF("CreateWindowExA: unknown class %s\n", cls.c_str());
      SetLastError(kErrorCannotFindWndClass);
      return 0;
    }
    win.hwnd = g_next_hwnd;
    g_next_hwnd += 0x10;
    win.cls = cls;
    win.wndproc = it->second.wndproc;
    win.class_style = it->second.style;
    win.extra.assign(it->second.wnd_extra, 0);
  }
  constexpr int32_t kUseDefault = int32_t(0x80000000);
  if (x == kUseDefault) x = y = 0;
  if (w == kUseDefault) {
    w = 640;
    h = 480;
  }
  win.style = style;
  win.exstyle = exstyle;
  win.parent = parent;
  win.id = (style & WS_CHILD) ? menu : 0;
  win.hinst = hinst;
  win.x = x;
  win.y = y;
  win.w = w;
  win.h = h;
  win.title = title_va ? GStr(title_va) : "";
  win.tid = CurrentThread()->tid;
  win.enabled = !(style & WS_DISABLED);
  win.z = g_z_bottom += 1024;
  bool host_window = !(style & WS_CHILD);
  if (host_window && platform::kSingleHostWindow) {
    // One window per display: a later top-level window (a dialog the game
    // draws itself, a helper) has no host window.
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (const auto& [h, other] : g_windows)
      if (other.sdl && !other.destroyed) host_window = false;
  }
  if (host_window) {
    EnsureSdlVideo();
    int cw, ch;
    ClientSize(win, &cw, &ch);
    win.sdl = SDL_CreateWindow(AnsiToUtf8(win.title).c_str(), ToPoints(nullptr, cw), ToPoints(nullptr, ch),
                               SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                   (FullscreenPolicy() == GameConfig::Fullscreen::Window
                                        ? SDL_WINDOW_RESIZABLE
                                        : 0) |
                                   GfxWindowFlags());
    if (!win.sdl) FatalF("SDL_CreateWindow: %s", SDL_GetError());
  }
  uint32_t hwnd = win.hwnd;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    g_windows[hwnd] = std::move(win);
  }
  RECOMP_TRACE("  CreateWindowExA(%s, \"%s\", style %08X, %d,%d %dx%d) -> %X\n",
            cls.c_str(), GStr(title_va).c_str(), style, x, y, w, h, hwnd);

  // CREATESTRUCTA in a process-heap block.
  uint32_t cs = HeapAllocProcess(48, true);
  Wr32(cs + 0, param);
  Wr32(cs + 4, hinst);
  Wr32(cs + 8, menu);
  Wr32(cs + 12, parent);
  Wr32(cs + 16, uint32_t(h));
  Wr32(cs + 20, uint32_t(w));
  Wr32(cs + 24, uint32_t(y));
  Wr32(cs + 28, uint32_t(x));
  Wr32(cs + 32, style);
  Wr32(cs + 36, title_va);
  Wr32(cs + 40, cls_va);
  Wr32(cs + 44, exstyle);
  bool ok = Send(hwnd, WM_NCCREATE, 0, cs) != 0;
  if (ok) ok = int32_t(Send(hwnd, WM_CREATE, 0, cs)) != -1;
  HeapFreeProcess(cs);
  if (!ok) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* wp = Win(hwnd)) {
      if (wp->sdl) SDL_DestroyWindow(wp->sdl);
      wp->destroyed = true;
    }
    return 0;
  }
  if (style & WS_VISIBLE) {
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      if (Window* wp = Win(hwnd)) {
        wp->visible = true;
        if (wp->sdl) SDL_ShowWindow(wp->sdl);
      }
    }
    UpdateFullscreen(hwnd);
    InvalidateTree(hwnd);
  }
  return hwnd;
}

HLE("user32.dll", CreateWindowExA, 12) {
  return CreateWindowCore(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), int32_t(Arg(c, 4)),
                          int32_t(Arg(c, 5)), int32_t(Arg(c, 6)), int32_t(Arg(c, 7)),
                          Arg(c, 8), Arg(c, 9), Arg(c, 10), Arg(c, 11));
}

}  // namespace
}  // namespace hle

uint32_t user32::CreateWindowHost(uint32_t exstyle, const std::string& cls,
                                  const std::string& title, uint32_t style, int x, int y, int w,
                                  int h, uint32_t parent, uint32_t menu, uint32_t hinst,
                                  uint32_t param) {
  // "#n" is the atom n (MAKEINTATOM); a name goes to guest memory, as the
  // window procedure sees it in CREATESTRUCT.
  gaddr cls_va = 0, title_va = HeapAllocProcess(title.size() + 1, false);
  PutStr(title_va, title);
  if (!cls.empty() && cls[0] == '#') {
    cls_va = gaddr(std::stoul(cls.substr(1)));
  } else {
    cls_va = HeapAllocProcess(cls.size() + 1, false);
    PutStr(cls_va, cls);
  }
  uint32_t hwnd = hle::CreateWindowCore(exstyle, uint32_t(cls_va), uint32_t(title_va), style, x,
                                        y, w, h, parent, menu, hinst, param);
  if (cls_va >= 0x10000) HeapFreeProcess(cls_va);
  HeapFreeProcess(title_va);
  return hwnd;
}

namespace hle {
namespace {

// WM_DESTROY goes to the window and then to its children; the children
// are gone (WM_NCDESTROY) before the window itself.
void DestroyTree(uint32_t hwnd) {
  uint32_t parent = 0;
  bool child = false;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return;
    child = w->style & WS_CHILD;
    parent = w->parent;
    if (g_capture && IsSelfOrAncestor(hwnd, g_capture)) g_capture = 0;
    if (g_focus && IsSelfOrAncestor(hwnd, g_focus)) g_focus = child ? parent : 0;
  }
  if (child) {
    // The area it covered is shown again.
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(hwnd); w && w->visible) {
      const int r[4] = {w->x, w->y, w->x + w->w, w->y + w->h};
      Invalidate(parent, r, true);
    }
  }
  Send(hwnd, WM_DESTROY, 0, 0);
  for (uint32_t ch : Children(hwnd)) DestroyTree(ch);
  Send(hwnd, WM_NCDESTROY, 0, 0);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd)) {
    if (g_clip.hwnd == hwnd) {
      // The clip stays; AttachClip gives it to the next window over it.
      g_clip.hwnd = 0;
      if (w->sdl) SDL_SetWindowRelativeMouseMode(w->sdl, false);
    }
    if (w->sdl) SDL_DestroyWindow(w->sdl);
    w->sdl = nullptr;
    w->destroyed = true;
  }
  std::erase_if(g_timers, [&](const Timer& t) { return t.hwnd == hwnd; });
  if (g_focus == hwnd) g_focus = 0;
  if (g_active == hwnd) g_active = 0;
}

HLE("user32.dll", DestroyWindow, 1) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!Win(Arg(c, 0))) return 0;
  }
  DestroyTree(Arg(c, 0));
  return 1;
}

HLE("user32.dll", IsWindow, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return Win(Arg(c, 0)) ? 1 : 0;
}

// A window just shown is painted whole; a child just hidden leaves its
// area to its parent.
void UncoverOrPaint(uint32_t hwnd, bool shown) {
  if (shown) {
    InvalidateTree(hwnd);
    return;
  }
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (w && (w->style & WS_CHILD)) {
    const int r[4] = {w->x, w->y, w->x + w->w, w->y + w->h};
    Invalidate(w->parent, r, true);
  }
}

HLE("user32.dll", ShowWindow, 2) {
  uint32_t hwnd = Arg(c, 0), cmd = Arg(c, 1);
  bool was, show = cmd != 0;  // SW_HIDE = 0
  // SW_SHOWMINIMIZED, SW_MINIMIZE, SW_SHOWMINNOACTIVE
  bool minimize = cmd == 2 || cmd == 6 || cmd == 7;
  // SW_SHOWNORMAL, SW_SHOWMAXIMIZED, SW_RESTORE, SW_SHOWDEFAULT
  bool restore = cmd == 1 || cmd == 3 || cmd == 9 || cmd == 10;
  // Only these make the window the active one; minimizing hands
  // activation to some other window (and the game minimizes itself from
  // its WM_ACTIVATEAPP(0) handler, so activating here would loop).
  bool activates = restore || cmd == 5;
  SDL_Window* sdl;
  bool exclusive;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    was = w->visible;
    w->visible = show;
    if (minimize) w->minimized = true;
    if (restore) w->minimized = false;
    sdl = w->sdl;
    exclusive = w->mode_w != 0;
  }
  if (sdl) {
    if (!show) {
      SDL_HideWindow(sdl);
    } else {
      SDL_ShowWindow(sdl);
      // Behind an exclusive-mode "screen" the game's minimize on
      // deactivation is not carried out: the window simply stays put
      // while the game idles, and focusing it again resumes the game.
      if (minimize && !exclusive) SDL_MinimizeWindow(sdl);
      if (restore && !exclusive) SDL_RestoreWindow(sdl);
    }
  }
  UpdateFullscreen(hwnd);
  if (show != was) {
    Send(hwnd, WM_SHOWWINDOW, show ? 1 : 0, 0);
    UncoverOrPaint(hwnd, show);
  }
  bool activate = false;
  {
    // A shown top-level window becomes the active app window. Record that
    // before sending: the game's activation handler calls ShowWindow
    // again, which must then see it as already active.
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (activates && sdl && g_active != hwnd) {
      g_active = g_focus = hwnd;
      activate = true;
    }
  }
  if (activate) {
    Send(hwnd, WM_ACTIVATEAPP, 1, 0);
    Send(hwnd, WM_ACTIVATE, 1, 0);
    Send(hwnd, WM_SETFOCUS, 0, 0);
  }
  return was ? 1 : 0;
}

// CloseWindow minimizes (it does not destroy).
HLE("user32.dll", CloseWindow, 1) {
  uint32_t show = ProcAddress(ModuleHandle("user32.dll"), "ShowWindow");
  CallGuest(c, show, {Arg(c, 0), 6 /* SW_MINIMIZE */});
  return 1;
}

// (hWnd): WM_PAINT now if the window has an update region.
HLE("user32.dll", UpdateWindow, 1) {
  PaintNow(Arg(c, 0));
  return 1;
}

// (hWnd, lpRect, bErase): hWnd NULL is every window.
HLE("user32.dll", InvalidateRect, 3) {
  int r[4];
  const int* pr = nullptr;
  if (Arg(c, 1)) {
    for (int i = 0; i < 4; i++) r[i] = int32_t(Rd32(Arg(c, 1) + 4 * i));
    pr = r;
  }
  if (!Arg(c, 0)) {
    std::vector<uint32_t> all;
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      for (auto& [h, w] : g_windows)
        if (!w.destroyed) all.push_back(h);
    }
    for (uint32_t h : all) Invalidate(h, nullptr, Arg(c, 2) != 0);
    return 1;
  }
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (!Win(Arg(c, 0))) return 0;
  Invalidate(Arg(c, 0), pr, Arg(c, 2) != 0);
  return 1;
}

HLE("user32.dll", IsWindowVisible, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w && w->visible ? 1 : 0;
}

HLE("user32.dll", IsWindowEnabled, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w && w->enabled ? 1 : 0;
}

// (hWnd, bEnable): WM_ENABLE when the state changes; a window that loses
// the focus by being disabled gives it up.
HLE("user32.dll", EnableWindow, 2) {
  const uint32_t hwnd = Arg(c, 0);
  const bool enable = Arg(c, 1) != 0;
  bool was_disabled;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    was_disabled = !w->enabled;
    w->enabled = enable;
    if (enable) w->style &= ~WS_DISABLED;
    else w->style |= WS_DISABLED;
    if (!enable && g_focus == hwnd) g_focus = 0;
  }
  if (was_disabled == enable) Send(hwnd, WM_ENABLE, enable ? 1 : 0, 0);
  return was_disabled ? 1 : 0;
}

HLE("user32.dll", IsIconic, 1) {
  SDL_Window* s;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    if (!w) return 0;
    if (w->minimized) return 1;
    s = w->sdl;
  }
  return s && (SDL_GetWindowFlags(s) & SDL_WINDOW_MINIMIZED) ? 1 : 0;
}


// Like Windows, WM_SIZE/WM_MOVE are only sent when the size/position
// actually changes (the game re-applies its rect from its WM_SIZE handler;
// sending unconditionally recurses forever).
void ApplyRect(uint32_t hwnd, int x, int y, int w, int h, bool move, bool size) {
  SDL_Window* sdl = nullptr;
  int cw = 0, ch = 0;
  int old[4] = {0, 0, 0, 0};  // the window rect before (parent coordinates)
  bool moved = false, sized = false, covers = false;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* win = Win(hwnd);
    if (!win) return;
    old[0] = win->x, old[1] = win->y, old[2] = win->x + win->w, old[3] = win->y + win->h;
    if (move && (win->x != x || win->y != y)) {
      win->x = x;
      win->y = y;
      moved = true;
    }
    // A drop-down combo box keeps the height of its selection field: a
    // taller height asked for is the field's and the drop-down list's
    // together; one no taller than the field leaves the list's height as
    // it was (games that move a combo box to its field's size still drop
    // the list the template gave).
    if (size && win->cls == "combobox" && (win->style & 0x3) != 0x1 /* CBS_SIMPLE */) {
      if (h > win->h) win->page = h - win->h;
      h = win->h;
    }
    if (size && (win->w != w || win->h != h)) {
      win->w = w;
      win->h = h;
      sized = true;
    }
    ClientSize(*win, &cw, &ch);
    // In exclusive mode the host window keeps the display-mode size, and
    // a fullscreen one the screen's.
    sdl = win->mode_w || win->fullscreen ? nullptr : win->sdl;
    covers = FullscreenPolicy() == GameConfig::Fullscreen::Window && CoversScreen(*win);
  }
  if (sdl && sized) SetHostSize(sdl, cw, ch, covers);
  if (moved || sized) UpdateFullscreen(hwnd);
  if (moved || sized) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* win = Win(hwnd);
    if (win && (win->style & WS_CHILD) && win->visible) {
      Invalidate(win->parent, old, true);
      InvalidateTree(hwnd);
    }
  }
  if (sized) Send(hwnd, WM_SIZE, 0, MkLparam(cw, ch));
  if (moved) Send(hwnd, WM_MOVE, 0, MkLparam(x, y));
}

// Puts child hwnd after `after` among its siblings: HWND_TOP (0) or
// HWND_TOPMOST (-1) first, HWND_BOTTOM (1) last.
void Restack(uint32_t hwnd, uint32_t after) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w || !(w->style & WS_CHILD)) return;
  std::vector<uint32_t> sibs = Children(w->parent);
  sibs.erase(std::remove(sibs.begin(), sibs.end(), hwnd), sibs.end());
  if (after == 0 || after == 0xFFFFFFFF) {
    sibs.insert(sibs.begin(), hwnd);
  } else if (after == 1 || after == 0xFFFFFFFE) {
    sibs.push_back(hwnd);
  } else {
    auto it = std::find(sibs.begin(), sibs.end(), after);
    sibs.insert(it == sibs.end() ? sibs.begin() : it + 1, hwnd);
  }
  for (size_t i = 0; i < sibs.size(); i++)
    if (Window* s = Win(sibs[i])) s->z = int64_t(i + 1) * 1024 - (int64_t(1) << 40);
}

}  // namespace
}  // namespace hle

void user32::RaiseChild(uint32_t hwnd) {
  hle::Restack(hwnd, 0);
  InvalidateTree(hwnd);
}

namespace hle {
namespace {

HLE("user32.dll", SetWindowPos, 7) {
  uint32_t hwnd = Arg(c, 0), flags = Arg(c, 6);
  bool nomove = flags & 0x2, nosize = flags & 0x1;
  if (!(flags & 0x4)) {  // SWP_NOZORDER
    Restack(hwnd, Arg(c, 1));
    InvalidateTree(hwnd);
  }
  if (flags & 0x80) {  // SWP_HIDEWINDOW
    bool was;
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      if (!w) return 0;
      was = w->visible;
      w->visible = false;
    }
    if (SDL_Window* s = SdlWindowFor(hwnd)) SDL_HideWindow(s);
    if (was) UncoverOrPaint(hwnd, false);
  }
  ApplyRect(hwnd, int32_t(Arg(c, 2)), int32_t(Arg(c, 3)), int32_t(Arg(c, 4)),
            int32_t(Arg(c, 5)), !nomove, !nosize);
  if (flags & 0x40) {  // SWP_SHOWWINDOW
    if (SDL_Window* s = SdlWindowFor(hwnd)) SDL_ShowWindow(s);
    bool was;
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      was = w && w->visible;
      if (w) w->visible = true;
    }
    UpdateFullscreen(hwnd);
    if (!was) UncoverOrPaint(hwnd, true);
  }
  return 1;
}

HLE("user32.dll", MoveWindow, 6) {
  ApplyRect(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)),
            int32_t(Arg(c, 3)), int32_t(Arg(c, 4)), true, true);
  return 1;
}

HLE("user32.dll", AdjustWindowRect, 3) {
  uint32_t rc = Arg(c, 0);
  int border, caption;
  FrameSize(Arg(c, 1), &border, &caption);
  Wr32(rc + 0, Rd32(rc + 0) - border);
  Wr32(rc + 4, Rd32(rc + 4) - border - caption);
  Wr32(rc + 8, Rd32(rc + 8) + border);
  Wr32(rc + 12, Rd32(rc + 12) + border);
  return 1;
}

// (lpRect, dwStyle, bMenu, dwExStyle): windows have no menu bars here, and
// the extended styles add no frame of their own.
HLE("user32.dll", AdjustWindowRectEx, 4) {
  uint32_t rc = Arg(c, 0);
  int border, caption;
  FrameSize(Arg(c, 1), &border, &caption);
  Wr32(rc + 0, Rd32(rc + 0) - border);
  Wr32(rc + 4, Rd32(rc + 4) - border - caption);
  Wr32(rc + 8, Rd32(rc + 8) + border);
  Wr32(rc + 12, Rd32(rc + 12) + border);
  return 1;
}

// (hWnd): no window has a menu.
HLE("user32.dll", GetMenu, 1) { return 0; }

// (hWnd, lpRect, bErase): the update rectangle, empty if there is none.
HLE("user32.dll", GetUpdateRect, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  const bool dirty = w && w->dirty;
  if (Arg(c, 1)) {
    const int r[4] = {dirty ? w->ul : 0, dirty ? w->ut : 0, dirty ? w->ur : 0, dirty ? w->ub : 0};
    for (int i = 0; i < 4; i++) Wr32(Arg(c, 1) + 4 * i, uint32_t(r[i]));
  }
  return dirty ? 1 : 0;
}

// (hWnd, lpRect): the update region is a rectangle here, so validating
// any part of it that covers it all empties it (a part leaves it whole).
HLE("user32.dll", ValidateRect, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  if (!w) return 0;
  w->paint_sent = false;
  if (!Arg(c, 1)) {
    w->dirty = w->erase = false;
    return 1;
  }
  int r[4];
  for (int i = 0; i < 4; i++) r[i] = int32_t(Rd32(Arg(c, 1) + 4 * i));
  if (r[0] <= w->ul && r[1] <= w->ut && r[2] >= w->ur && r[3] >= w->ub)
    w->dirty = w->erase = false;
  return 1;
}

// (hWnd, lprcUpdate, hrgnUpdate, flags): RDW_INVALIDATE 0x1, RDW_ERASE 0x4,
// RDW_VALIDATE 0x8, RDW_ALLCHILDREN 0x80, RDW_UPDATENOW 0x100,
// RDW_ERASENOW 0x200. Regions are not modelled: hrgnUpdate means all.
HLE("user32.dll", RedrawWindow, 4) {
  const uint32_t hwnd = Arg(c, 0), flags = Arg(c, 3);
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!Win(hwnd)) return 0;
  }
  int r[4];
  const int* pr = nullptr;
  if (Arg(c, 1) && !Arg(c, 2)) {
    for (int i = 0; i < 4; i++) r[i] = int32_t(Rd32(Arg(c, 1) + 4 * i));
    pr = r;
  }
  if (flags & 0x1) {
    if (flags & 0x80)
      InvalidateTree(hwnd);
    else
      Invalidate(hwnd, pr, flags & 0x4);
  }
  if (flags & 0x8) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(hwnd)) w->dirty = w->erase = false;
  }
  if (flags & 0x300) {
    PaintNow(hwnd);
    if (flags & 0x80)
      for (uint32_t ch : Children(hwnd)) PaintNow(ch);
  }
  return 1;
}
// (hWndLock): nothing is drawn by the window manager to hold back.
HLE("user32.dll", LockWindowUpdate, 1) { return 1; }

// (lprc, xLeft, yTop, xRight, yBottom)
HLE("user32.dll", SetRect, 5) {
  const gaddr rc = Arg(c, 0);
  if (!rc) return 0;
  for (int i = 0; i < 4; i++) Wr32(rc + 4 * i, uint32_t(Arg(c, 1 + i)));
  return 1;
}

// (lprcDst, lprcSrc1, lprcSrc2): an empty intersection sets all four to 0
// and returns FALSE.
HLE("user32.dll", IntersectRect, 3) {
  const gaddr d = Arg(c, 0), a = Arg(c, 1), b = Arg(c, 2);
  if (!d || !a || !b) return 0;
  auto rd = [](gaddr r, int i) { return int32_t(Rd32(r + 4 * i)); };
  RECOMP_TRACE("  IntersectRect (%d,%d,%d,%d) & (%d,%d,%d,%d)\n", rd(a, 0), rd(a, 1), rd(a, 2),
               rd(a, 3), rd(b, 0), rd(b, 1), rd(b, 2), rd(b, 3));
  const int32_t l = std::max(rd(a, 0), rd(b, 0)), t = std::max(rd(a, 1), rd(b, 1));
  const int32_t r = std::min(rd(a, 2), rd(b, 2)), btm = std::min(rd(a, 3), rd(b, 3));
  if (l >= r || t >= btm) {
    for (int i = 0; i < 4; i++) Wr32(d + 4 * i, 0);
    return 0;
  }
  Wr32(d, uint32_t(l));
  Wr32(d + 4, uint32_t(t));
  Wr32(d + 8, uint32_t(r));
  Wr32(d + 12, uint32_t(btm));
  return 1;
}

// (hWnd, lpClassName, nMaxCount): characters copied, without the NUL.
HLE("user32.dll", GetClassNameA, 3) {
  std::string cls;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    if (!w) {
      SetLastError(1400);  // ERROR_INVALID_WINDOW_HANDLE
      return 0;
    }
    auto k = g_classes.find(w->cls);
    cls = k != g_classes.end() && !k->second.display.empty() ? k->second.display : w->cls;
  }
  const uint32_t cap = Arg(c, 2);
  if (!cap) return 0;
  cls = cls.substr(0, cap - 1);
  PutStr(Arg(c, 1), cls);
  return uint32_t(cls.size());
}

// In an exclusive display mode the window is as large as the mode's
// screen (DirectDraw sizes the focus window to it when the mode is set):
// games centre what they draw on the primary (movies) in this rectangle.
HLE("user32.dll", GetClientRect, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  uint32_t rc = Arg(c, 1);
  if (!w) return 0;
  int cw, ch;
  GameClientSize(*w, &cw, &ch);
  Wr32(rc, 0);
  Wr32(rc + 4, 0);
  Wr32(rc + 8, uint32_t(cw));
  Wr32(rc + 12, uint32_t(ch));
  return 1;
}

HLE("user32.dll", GetWindowRect, 2) {
  uint32_t hwnd = Arg(c, 0), rc = Arg(c, 1);
  if (hwnd == 0x00010000) {  // desktop
    Wr32(rc, 0);
    Wr32(rc + 4, 0);
    Wr32(rc + 8, uint32_t(g_display_w));
    Wr32(rc + 12, uint32_t(g_display_h));
    return 1;
  }
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w) return 0;
  int x = w->x, y = w->y;
  if ((w->style & WS_CHILD) && Win(w->parent)) {
    int px, py;
    WindowOrigin(w->parent, &px, &py);
    x += px;
    y += py;
  }
  Wr32(rc, uint32_t(x));
  Wr32(rc + 4, uint32_t(y));
  Wr32(rc + 8, uint32_t(x + w->w));
  Wr32(rc + 12, uint32_t(y + w->h));
  RECOMP_TRACE("  GetWindowRect -> (%d,%d,%d,%d)\n", x, y, x + w->w, y + w->h);
  return 1;
}

HLE("user32.dll", ClientToScreen, 2) {
  int ox, oy;
  WindowOrigin(Arg(c, 0), &ox, &oy);
  uint32_t pt = Arg(c, 1);
  Wr32(pt, Rd32(pt) + uint32_t(ox));
  Wr32(pt + 4, Rd32(pt + 4) + uint32_t(oy));
  return 1;
}

HLE("user32.dll", ScreenToClient, 2) {
  int ox, oy;
  WindowOrigin(Arg(c, 0), &ox, &oy);
  uint32_t pt = Arg(c, 1);
  Wr32(pt, Rd32(pt) - uint32_t(ox));
  Wr32(pt + 4, Rd32(pt + 4) - uint32_t(oy));
  return 1;
}

HLE("user32.dll", MapWindowPoints, 4) {
  int fx, fy, tx, ty;
  WindowOrigin(Arg(c, 0), &fx, &fy);
  WindowOrigin(Arg(c, 1), &tx, &ty);
  uint32_t pts = Arg(c, 2), n = Arg(c, 3);
  int dx = fx - tx, dy = fy - ty;
  for (uint32_t i = 0; i < n * 2; i += 2) {
    Wr32(pts + 4 * i, Rd32(pts + 4 * i) + uint32_t(dx));
    Wr32(pts + 4 * i + 4, Rd32(pts + 4 * i + 4) + uint32_t(dy));
  }
  return MkLparam(dx, dy);
}

HLE("user32.dll", GetDesktopWindow, 0) { return 0x00010000; }

HLE("user32.dll", GetParent, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w ? w->parent : 0;
}

// (hWnd, uCmd): GW_HWNDFIRST 0, GW_HWNDLAST 1, GW_HWNDNEXT 2, GW_HWNDPREV
// 3, GW_OWNER 4, GW_CHILD 5. Siblings are a window's parent's children
// (for top-level windows, the other top-level windows) in Z order.
HLE("user32.dll", GetWindow, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  const uint32_t hwnd = Arg(c, 0), cmd = Arg(c, 1);
  Window* w = Win(hwnd);
  if (!w) return 0;
  if (cmd == 4) return (w->style & WS_CHILD) ? 0 : w->parent;
  if (cmd == 5) {
    std::vector<uint32_t> kids = Children(hwnd);
    return kids.empty() ? 0 : kids.front();
  }
  std::vector<uint32_t> sibs;
  if (w->style & WS_CHILD) {
    sibs = Children(w->parent);
  } else {
    for (auto& [h, o] : g_windows)
      if (!o.destroyed && !(o.style & WS_CHILD)) sibs.push_back(h);
  }
  auto it = std::find(sibs.begin(), sibs.end(), hwnd);
  switch (cmd) {
    case 0: return sibs.empty() ? 0 : sibs.front();
    case 1: return sibs.empty() ? 0 : sibs.back();
    case 2: return it != sibs.end() && it + 1 != sibs.end() ? *(it + 1) : 0;
    case 3: return it != sibs.end() && it != sibs.begin() ? *(it - 1) : 0;
    default: return 0;
  }
}

HLE("user32.dll", GetLastActivePopup, 1) { return Arg(c, 0); }

HLE("user32.dll", FindWindowA, 2) {
  std::string cls = Arg(c, 0) ? Lower(GStr(Arg(c, 0))) : "";
  std::string title = Arg(c, 1) ? GStr(Arg(c, 1)) : "";
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows) {
    if (w.destroyed || (w.style & WS_CHILD)) continue;
    if (Arg(c, 0) && w.cls != cls) continue;
    if (Arg(c, 1) && w.title != title) continue;
    return h;
  }
  return 0;
}

HLE("user32.dll", SetWindowTextA, 2) {
  std::string t = GStr(Arg(c, 1));
  SDL_Window* s = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    if (!w) return 0;
    w->title = t;
    s = w->sdl;
  }
  if (s) SDL_SetWindowTitle(s, AnsiToUtf8(t).c_str());
  return 1;
}

HLE("user32.dll", GetWindowTextA, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  uint32_t buf = Arg(c, 1), cap = Arg(c, 2);
  if (!w || !cap) return 0;
  std::string t = w->title.substr(0, cap - 1);
  PutStr(buf, t);
  return uint32_t(t.size());
}

HLE("user32.dll", GetWindowTextLengthA, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w ? uint32_t(w->title.size()) : 0;
}

HLE("user32.dll", GetWindowLongA, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  int32_t idx = int32_t(Arg(c, 1));
  if (!w) return 0;
  switch (idx) {
    case GWL_WNDPROC: return w->wndproc;
    case GWL_HINSTANCE: return w->hinst;
    case GWL_HWNDPARENT: return w->parent;
    case GWL_STYLE: return w->style | (w->visible ? WS_VISIBLE : 0);
    case GWL_EXSTYLE: return w->exstyle;
    case GWL_USERDATA: return w->userdata;
    case GWL_ID: return w->id;
  }
  if (idx >= 0 && uint32_t(idx) + 4 <= w->extra.size()) {
    uint32_t v;
    std::memcpy(&v, w->extra.data() + idx, 4);
    return v;
  }
  return 0;
}

HLE("user32.dll", SetWindowLongA, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  int32_t idx = int32_t(Arg(c, 1));
  uint32_t v = Arg(c, 2), old = 0;
  if (!w) return 0;
  switch (idx) {
    case GWL_WNDPROC: old = w->wndproc; w->wndproc = v; return old;
    case GWL_HWNDPARENT: old = w->parent; w->parent = v; return old;
    case GWL_STYLE: old = w->style; w->style = v; return old;
    case GWL_EXSTYLE: old = w->exstyle; w->exstyle = v; return old;
    case GWL_USERDATA: old = w->userdata; w->userdata = v; return old;
    case GWL_ID: old = w->id; w->id = v; return old;
  }
  if (idx >= 0 && uint32_t(idx) + 4 <= w->extra.size()) {
    std::memcpy(&old, w->extra.data() + idx, 4);
    std::memcpy(w->extra.data() + idx, &v, 4);
  }
  return old;
}

// ---- focus and activation ---------------------------------------------------------

HLE("user32.dll", GetFocus, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_focus;
}
// (hWnd): WM_KILLFOCUS to the window losing the focus, then WM_SETFOCUS to
// the one gaining it, each with the other's handle.
HLE("user32.dll", SetFocus, 1) {
  const uint32_t to = Arg(c, 0);
  uint32_t old;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    old = g_focus;
    if (to && !Win(to)) return 0;
    if (old == to) return old;
    g_focus = to;
  }
  if (old) Send(old, WM_KILLFOCUS, to, 0);
  if (to) Send(to, WM_SETFOCUS, old, 0);
  return old;
}
HLE("user32.dll", GetActiveWindow, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_active;
}
HLE("user32.dll", SetActiveWindow, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t old = g_active;
  g_active = Arg(c, 0);
  return old;
}
HLE("user32.dll", GetForegroundWindow, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_active;
}
HLE("user32.dll", SetForegroundWindow, 1) {
  if (SDL_Window* s = SdlWindowFor(Arg(c, 0))) SDL_RaiseWindow(s);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  g_active = g_focus = Arg(c, 0);
  return 1;
}
HLE("user32.dll", BringWindowToTop, 1) {
  if (SDL_Window* s = SdlWindowFor(Arg(c, 0))) SDL_RaiseWindow(s);
  return 1;
}

// ---- messages ----------------------------------------------------------------------

// Games that wait for their next frame by spinning on PeekMessage (an
// empty queue, again and again) keep a host core busy pumping the host's
// event loop. When a thread's previous PeekMessage also found nothing and
// was under kIdleSpinUs ago, the call first waits up to kIdleWaitUs for
// host events (the main thread) or a posted message: an arriving event
// ends the wait at once, so input is not delayed, and a frame loop that
// does work between its peeks never waits.
constexpr int64_t kIdleSpinUs = 2000, kIdleWaitUs = 1000;
thread_local int64_t t_last_empty_peek_us = -1;  // -1: the last peek found a message

void IdleYield() {
  // <ID>_IDLE_YIELD=0 turns it off (for measuring its effect).
  static const bool on = [] {
    const char* e = Env("IDLE_YIELD");
    return !(e && *e == '0');
  }();
  if (!on) return;
  int64_t& last_empty_us = t_last_empty_peek_us;
  const int64_t now = int64_t(SDL_GetTicksNS() / 1000);
  const bool spinning = last_empty_us >= 0 && now - last_empty_us < kIdleSpinUs;
  last_empty_us = now;
  if (!spinning) return;
  if (OnMainThread() && g_sdl_up) {
    SDL_WaitEventTimeout(nullptr, int32_t(kIdleWaitUs / 1000));
  } else {
    std::unique_lock<std::recursive_mutex> l(g_mu);
    g_queue_cv.wait_for(l, std::chrono::microseconds(kIdleWaitUs));
  }
}

HLE("user32.dll", PeekMessageA, 5) {
  uint32_t msg_va = Arg(c, 0), hwnd = Arg(c, 1), lo = Arg(c, 2), hi = Arg(c, 3),
           flags = Arg(c, 4);
  PumpHostEvents();
  Msg m;
  if (!TakeMessage(hwnd, lo, hi, flags & 1 /* PM_REMOVE */, &m)) {
    IdleYield();
    return 0;
  }
  t_last_empty_peek_us = -1;
  PutMsg(msg_va, m);
  return 1;
}

HLE("user32.dll", GetMessageA, 4) {
  uint32_t msg_va = Arg(c, 0), hwnd = Arg(c, 1), lo = Arg(c, 2), hi = Arg(c, 3);
  Msg m;
  for (;;) {
    PumpHostEvents();
    if (TakeMessage(hwnd, lo, hi, true, &m)) break;
    if (OnMainThread() && g_sdl_up) {
      SDL_WaitEventTimeout(nullptr, 10);
    } else {
      std::unique_lock<std::recursive_mutex> l(g_mu);
      g_queue_cv.wait_for(l, std::chrono::milliseconds(10));
    }
  }
  PutMsg(msg_va, m);
  return m.msg == WM_QUIT ? 0 : 1;
}

HLE("user32.dll", PostMessageA, 4) {
  uint32_t hwnd = Arg(c, 0);
  if (!hwnd) {
    PostTo(CurrentThread()->tid,
           Msg{0, uint32_t(Arg(c, 1)), uint32_t(Arg(c, 2)), uint32_t(Arg(c, 3)), TickCount(),
               g_mouse_x, g_mouse_y});
    return 1;
  }
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!Win(hwnd)) return 0;
  }
  Post(hwnd, Arg(c, 1), Arg(c, 2), Arg(c, 3));
  return 1;
}

// (idThread, Msg, wParam, lParam): a message with no window in that
// thread's queue (GetMessage/PeekMessage with any filter window of NULL).
HLE("user32.dll", PostThreadMessageA, 4) {
  PostTo(uint32_t(Arg(c, 0)), Msg{0, uint32_t(Arg(c, 1)), uint32_t(Arg(c, 2)),
                                  uint32_t(Arg(c, 3)), TickCount(), g_mouse_x, g_mouse_y});
  return 1;
}

HLE("user32.dll", PostQuitMessage, 1) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Queue& q = g_queues[CurrentThread()->tid];
    q.quit = true;
    q.quit_code = Arg(c, 0);
  }
  g_queue_cv.notify_all();
  return 0;
}

HLE("user32.dll", SendMessageA, 4) {
  return Send(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3));
}

HLE("user32.dll", DispatchMessageA, 1) {
  uint32_t m = Arg(c, 0);
  uint32_t hwnd = Rd32(m), msg = Rd32(m + 4);
  // WM_TIMER from a timer with a TIMERPROC goes to the procedure.
  if (msg == WM_TIMER && Rd32(m + 12))
    return CallGuest(c, Rd32(m + 12), {hwnd, msg, Rd32(m + 8), Rd32(m + 16)});
  if (!hwnd) return 0;
  return Send(hwnd, msg, Rd32(m + 8), Rd32(m + 12));
}

// ---- hot keys ------------------------------------------------------------------------

HLE("user32.dll", RegisterHotKey, 4) {
  uint32_t hwnd = Arg(c, 0), id = Arg(c, 1), mods = Arg(c, 2), vk = Arg(c, 3);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t tid = CurrentThread()->tid;
  if (hwnd) {
    Window* w = Win(hwnd);
    if (!w) return 0;
    tid = w->tid;
  }
  for (const HotKey& k : g_hotkeys)
    if ((k.hwnd == hwnd && k.id == id) || (k.vk == vk && (k.mods & 7) == (mods & 7))) {
      SetLastError(1409);  // ERROR_HOTKEY_ALREADY_REGISTERED
      return 0;
    }
  g_hotkeys.push_back({hwnd, id, mods, vk, tid});
  return 1;
}

HLE("user32.dll", UnregisterHotKey, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = std::find_if(g_hotkeys.begin(), g_hotkeys.end(), [&](const HotKey& k) {
    return k.hwnd == Arg(c, 0) && k.id == Arg(c, 1);
  });
  if (it == g_hotkeys.end()) {
    SetLastError(1419);  // ERROR_HOTKEY_NOT_REGISTERED
    return 0;
  }
  g_hotkeys.erase(it);
  return 1;
}

// ---- timers ------------------------------------------------------------------------

HLE("user32.dll", SetTimer, 4) {
  uint32_t hwnd = Arg(c, 0), id = Arg(c, 1), elapse = Arg(c, 2), proc = Arg(c, 3);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t tid = CurrentThread()->tid;
  if (hwnd) {
    Window* w = Win(hwnd);
    if (!w) return 0;
    tid = w->tid;
  } else {
    // A thread timer: the system picks the id.
    static uint32_t next_id = 0x7F00;
    id = ++next_id;
  }
  elapse = std::clamp<uint32_t>(elapse, 10, 0x7FFFFFFF);  // USER_TIMER_MINIMUM
  for (Timer& t : g_timers)
    if (t.hwnd == hwnd && t.id == id) {  // same timer: reset it
      t = Timer{hwnd, id, elapse, proc, tid, int64_t(TickCount()) + elapse};
      return hwnd ? 1 : id;
    }
  g_timers.push_back(Timer{hwnd, id, elapse, proc, tid, int64_t(TickCount()) + elapse});
  return hwnd ? 1 : id;
}

HLE("user32.dll", KillTimer, 2) {
  uint32_t hwnd = Arg(c, 0), id = Arg(c, 1);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = std::find_if(g_timers.begin(), g_timers.end(),
                         [&](const Timer& t) { return t.hwnd == hwnd && t.id == id; });
  if (it == g_timers.end()) return 0;
  g_timers.erase(it);
  return 1;
}

HLE("user32.dll", TranslateMessage, 1) {
  // Generate WM_CHAR from WM_KEYDOWN using the US layout and shift/caps.
  uint32_t m = Arg(c, 0);
  uint32_t msg = Rd32(m + 4), vk = Rd32(m + 8);
  if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return 0;
  bool shift = g_key_down[0x10], caps = g_key_toggled[0x14];
  bool ctrl = g_key_down[0x11];
  uint32_t ch = 0;
  if (vk >= 'A' && vk <= 'Z') {
    ch = (shift != caps) ? vk : vk + 32;
    if (ctrl) ch = vk - 'A' + 1;
  } else if (vk >= 0x60 && vk <= 0x69 && g_key_toggled[0x90]) {
    ch = '0' + (vk - 0x60);
  } else {
    static const struct { uint8_t vk; char n, s; } k[] = {
        {'0', '0', ')'}, {'1', '1', '!'}, {'2', '2', '@'}, {'3', '3', '#'},
        {'4', '4', '$'}, {'5', '5', '%'}, {'6', '6', '^'}, {'7', '7', '&'},
        {'8', '8', '*'}, {'9', '9', '('}, {0xBD, '-', '_'}, {0xBB, '=', '+'},
        {0xDB, '[', '{'}, {0xDD, ']', '}'}, {0xDC, '\\', '|'},
        {0xBA, ';', ':'}, {0xDE, '\'', '"'}, {0xC0, '`', '~'},
        {0xBC, ',', '<'}, {0xBE, '.', '>'}, {0xBF, '/', '?'},
        {0x20, ' ', ' '}, {0x0D, 13, 13}, {0x08, 8, 8}, {0x09, 9, 9},
        {0x1B, 27, 27}, {0x6A, '*', '*'}, {0x6B, '+', '+'},
        {0x6D, '-', '-'}, {0x6F, '/', '/'}, {0x6E, '.', '.'}};
    for (const auto& e : k)
      if (e.vk == vk) ch = uint8_t(shift ? e.s : e.n);
  }
  if (!ch) return 0;
  Post(Rd32(m), msg == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, ch, Rd32(m + 12));
  return 1;
}

HLE("user32.dll", TranslateAcceleratorA, 3) { return 0; }

HLE("user32.dll", CallWindowProcA, 5) {
  return CallGuest(c, Arg(c, 0), {Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4)});
}

HLE("user32.dll", DefWindowProcA, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2);
  switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: {
      uint32_t destroy = ProcAddress(ModuleHandle("user32.dll"), "DestroyWindow");
      CallGuest(c, destroy, {hwnd});
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {  // BeginPaint/EndPaint: the update region is validated
      std::lock_guard<std::recursive_mutex> l(g_mu);
      if (Window* w = Win(hwnd)) w->dirty = w->erase = false;
      return 0;
    }
    case WM_NCHITTEST: return 1;  // HTCLIENT
    case WM_MOUSEACTIVATE: return 1;  // MA_ACTIVATE
    case WM_SETCURSOR: return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == 0xF060) Post(hwnd, WM_CLOSE, 0, 0);  // SC_CLOSE
      return 0;
    case WM_SYSKEYDOWN:
      if (wp == 0x73 /* VK_F4 */ && g_key_down[0x12]) Post(hwnd, WM_CLOSE, 0, 0);
      return 0;
    case WM_GETTEXTLENGTH: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      return w ? uint32_t(w->title.size()) : 0;
    }
    default:
      return 0;
  }
}

}  // namespace
}  // namespace hle

uint32_t user32::CallDefWindowProc(Ctx& c, uint32_t hwnd, uint32_t msg, uint32_t wp,
                                   uint32_t lp) {
  uint32_t def = hle::ProcAddress(hle::ModuleHandle("user32.dll"), "DefWindowProcA");
  return CallGuest(c, def, {hwnd, msg, wp, lp});
}

namespace hle {
namespace {

HLE("user32.dll", MsgWaitForMultipleObjects, 5) {
  // Wait for objects or for any queued input/message (wake mask ignored).
  uint32_t n = Arg(c, 0), arr = Arg(c, 1), wait_all = Arg(c, 2),
           timeout = Arg(c, 3);
  uint32_t wfm = ProcAddress(ModuleHandle("kernel32.dll"), "WaitForMultipleObjects");
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
  for (;;) {
    PumpHostEvents();
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Queue& q = g_queues[CurrentThread()->tid];
      if (!q.msgs.empty() || q.quit) return n;  // WAIT_OBJECT_0 + n
    }
    if (n) {
      uint32_t r = CallGuest(c, wfm, {n, arr, wait_all, 0});
      if (r != 0x102) return r;
    }
    if (timeout != 0xFFFFFFFF && std::chrono::steady_clock::now() >= deadline)
      return 0x102;  // WAIT_TIMEOUT
    if (OnMainThread() && g_sdl_up)
      SDL_WaitEventTimeout(nullptr, 1);
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// ---- hooks ---------------------------------------------------------------------------
// The only hook is WH_KEYBOARD_LL (13), installed to swallow the Windows
// keys. Our input never produces system hotkeys, so the hook gets a handle
// and is never called.

HLE64("user32.dll", SetWindowsHookExA, 4) {
  static uint32_t next = 0x00F00010;
  RECOMP_TRACE("  SetWindowsHookExA(type %d, proc %08llX): ignored\n", int32_t(Arg(c, 0)),
               (unsigned long long)Arg(c, 1));
  return HandleRet(next += 0x10);
}
HLE64("user32.dll", UnhookWindowsHookEx, 1) { return 1; }
HLE64("user32.dll", CallNextHookEx, 4) { return 0; }

// ---- cursor ------------------------------------------------------------------------

HLE("user32.dll", ShowCursor, 1) {
  g_cursor_count += Arg(c, 0) ? 1 : -1;
  if (g_sdl_up && !Background()) {
    if (g_cursor_count >= 0)
      SDL_ShowCursor();
    else
      SDL_HideCursor();
  }
  return uint32_t(g_cursor_count);
}

// Some games re-centre the pointer every frame and move their own cursor
// by trunc((pos - ref) * sensitivity), clamped to the screen, where ref is
// the last position they read or set (some do in fullscreen). Over
// a desktop window that drifts from the host pointer, so while the pointer
// is over the window and the game is re-centring, GetCursorPos returns the
// position that lands the game's cursor exactly under the pointer.
// GameConfig::game_cursor says where that cursor is.
bool SyncedCursorPos(int32_t* sx, int32_t* sy) {
  if (!g_pointer_inside || !Game().game_cursor) return false;
  // Only while the game is re-centring (otherwise it reads absolute
  // positions and the plain pointer position is right).
  if (int64_t(TickCount()) - g_last_recentre_ms > 250) return false;
  uint32_t app = 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    for (auto& [h, w] : g_windows)
      if (w.mode_w && !w.destroyed) app = w.userdata;
  }
  int32_t gx, gy;
  float kx = 1, ky = 1;
  if (!app || !Game().game_cursor(app, &gx, &gy, &kx, &ky)) return false;
  int32_t dx = int32_t(std::lround((g_real_x - gx) / kx));
  int32_t dy = int32_t(std::lround((g_real_y - gy) / ky));
  *sx = g_recentre_x + dx;
  *sy = g_recentre_y + dy;
  return true;
}

// ChildWindowFromPoint(Ex)(hwnd, POINT pt, flags): the immediate child of
// hwnd (topmost first) whose window rect holds pt (client coordinates of
// hwnd), hwnd itself if none does, or NULL when pt is outside hwnd's
// client area. The POINT is passed by value (two stack slots).
constexpr uint32_t CWP_SKIPINVISIBLE = 1, CWP_SKIPDISABLED = 2, CWP_SKIPTRANSPARENT = 4;

uint32_t ChildFromPoint(uint32_t hwnd, int x, int y, uint32_t flags) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* parent = Win(hwnd);
  if (!parent) return 0;
  int cw, ch;
  ClientSize(*parent, &cw, &ch);
  if (x < 0 || y < 0 || x >= cw || y >= ch) return 0;
  for (uint32_t c : Children(hwnd)) {
    Window* w = Win(c);
    if (!w) continue;
    if ((flags & CWP_SKIPINVISIBLE) && !w->visible) continue;
    if ((flags & CWP_SKIPDISABLED) && !w->enabled) continue;
    if ((flags & CWP_SKIPTRANSPARENT) && (w->exstyle & 0x20 /* WS_EX_TRANSPARENT */)) continue;
    if (x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h) return c;
  }
  return hwnd;
}

HLE("user32.dll", ChildWindowFromPoint, 3) {
  return ChildFromPoint(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), 0);
}

HLE("user32.dll", ChildWindowFromPointEx, 4) {
  return ChildFromPoint(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), Arg(c, 3));
}

// WindowFromPoint(POINT pt): the visible window at screen point pt: the
// topmost visible top-level window holding it, then the deepest visible
// child there (static text and group boxes let the window below through,
// as their HTTRANSPARENT hit test does); a disabled child yields its
// parent.
HLE("user32.dll", WindowFromPoint, 2) {
  const int x = int32_t(Arg(c, 0)), y = int32_t(Arg(c, 1));
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t top = 0;
  int64_t top_z = 0;
  for (auto& [h, w] : g_windows) {
    if (w.destroyed || (w.style & WS_CHILD) || !w.visible) continue;
    int ox, oy, cw, ch;
    WindowOrigin(h, &ox, &oy);
    ClientSize(w, &cw, &ch);
    if (x < ox || y < oy || x >= ox + cw || y >= oy + ch) continue;
    if (!top || w.z < top_z) top = h, top_z = w.z;
  }
  if (!top) return 0;
  uint32_t hit = ChildAtScreen(top, x, y);
  while (hit != top) {
    Window* w = Win(hit);
    if (w && w->enabled) break;
    hit = w ? w->parent : top;
  }
  return hit;
}

HLE("user32.dll", GetCursorPos, 1) {
  int32_t x = g_mouse_x, y = g_mouse_y;
  SyncedCursorPos(&x, &y);
  Wr32(Arg(c, 0), uint32_t(x));
  Wr32(Arg(c, 0) + 4, uint32_t(y));
  return 1;
}

HLE("user32.dll", SetCursorPos, 2) {
  g_mouse_x = g_recentre_x = int32_t(Arg(c, 0));
  g_mouse_y = g_recentre_y = int32_t(Arg(c, 1));
  g_last_recentre_ms = TickCount();
  return 1;
}

// ClipCursor(RECT const*): NULL frees the cursor. A rectangle inside one
// of our windows makes its cursor virtual and grabs the host pointer.
HLE("user32.dll", ClipCursor, 1) {
  uint32_t rc = Arg(c, 0);
  SDL_Window* release = nullptr;
  SDL_Window* grab = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (g_clip.on && g_clip.hwnd)
      if (Window* w = Win(g_clip.hwnd)) release = w->sdl;
    g_clip = {};
    if (rc) {
      g_clip.on = true;
      g_clip.l = int32_t(Rd32(rc));
      g_clip.t = int32_t(Rd32(rc + 4));
      g_clip.r = int32_t(Rd32(rc + 8));
      g_clip.b = int32_t(Rd32(rc + 12));
      g_clip.hwnd = ClipWindow();
      if (Window* w = Win(g_clip.hwnd)) grab = w->sdl;
      g_mouse_x = std::clamp(g_mouse_x, g_clip.l, std::max(g_clip.l, g_clip.r - 1));
      g_mouse_y = std::clamp(g_mouse_y, g_clip.t, std::max(g_clip.t, g_clip.b - 1));
    }
  }
  if (release && release != grab) SDL_SetWindowRelativeMouseMode(release, false);
  if (grab && !Background()) SDL_SetWindowRelativeMouseMode(grab, true);
  return 1;
}

// Mouse capture: there is one game window, so mouse messages go to it
// either way; the capture window is only remembered.
HLE("user32.dll", SetCapture, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t old = g_capture;
  g_capture = Arg(c, 0);
  return old;
}

HLE("user32.dll", GetCapture, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_capture;
}

HLE("user32.dll", ReleaseCapture, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  g_capture = 0;
  return 1;
}

// ---- display modes -------------------------------------------------------------------

struct Mode {
  int w, h, bpp;
};

std::vector<Mode> Modes() {
  std::vector<Mode> m;
  for (int bpp : {16, 32})
    for (const ModeSize& s : DisplayModeSizes()) m.push_back({s.w, s.h, bpp});
  return m;
}

void PutDevMode(uint32_t dm, const Mode& m) {
  std::memset(GPtr(dm), 0, 156);
  PutStr(dm, win32::Upper(Game().id) + " Display");
  Wr16(dm + 32, 0x0401);  // dmSpecVersion
  Wr16(dm + 36, 156);     // dmSize
  Wr32(dm + 40, 0x001C0000 | 0x00400000);  // BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY
  Wr32(dm + 104, uint32_t(m.bpp));
  Wr32(dm + 108, uint32_t(m.w));
  Wr32(dm + 112, uint32_t(m.h));
  Wr32(dm + 120, 60);
}

HLE("user32.dll", EnumDisplaySettingsA, 3) {
  EnsureSdlVideo();
  uint32_t idx = Arg(c, 1), dm = Arg(c, 2);
  if (idx == 0xFFFFFFFF || idx == 0xFFFFFFFE) {  // ENUM_CURRENT/REGISTRY_SETTINGS
    PutDevMode(dm, {g_display_w, g_display_h, 32});
    return 1;
  }
  auto modes = Modes();
  if (idx >= modes.size()) return 0;
  PutDevMode(dm, modes[idx]);
  return 1;
}

// Display modes are virtual: the host's mode never changes here. A mode
// the game switches to becomes the screen it sees (GetSystemMetrics,
// EnumDisplaySettings), as after a real switch; a window covering it goes
// host fullscreen (UpdateFullscreen). NULL restores the desktop size.
HLE("user32.dll", ChangeDisplaySettingsA, 2) {
  EnsureSdlVideo();
  uint32_t dm = Arg(c, 0), flags = Arg(c, 1);
  if (!dm) {
    g_display_w = g_desktop_w;
    g_display_h = g_desktop_h;
    UpdateFullscreenAll();
    return 0;  // DISP_CHANGE_SUCCESSFUL
  }
  uint32_t fields = Rd32(dm + 40);
  int w = (fields & 0x00080000) ? int(Rd32(dm + 108)) : g_display_w;  // DM_PELSWIDTH
  int h = (fields & 0x00100000) ? int(Rd32(dm + 112)) : g_display_h;  // DM_PELSHEIGHT
  int bpp = (fields & 0x00040000) ? int(Rd32(dm + 104)) : 32;         // DM_BITSPERPEL
  bool known = false;
  for (const Mode& m : Modes()) known |= m.w == w && m.h == h && m.bpp == bpp;
  if (!known) return uint32_t(-2);  // DISP_CHANGE_BADMODE
  if (flags & 2) return 0;          // CDS_TEST
  g_display_w = w;
  g_display_h = h;
  UpdateFullscreenAll();
  return 0;
}

// ---- painting and GDI device contexts (no GDI drawing reaches the screen;
// the game renders through DirectDraw) -------------------------------------------------

// Device contexts are named after their window (see DcForWindow); the
// screen DC (GetDC(NULL)) is a constant.
HLE("user32.dll", GetDC, 1) { return DcForWindow(Arg(c, 0)); }
HLE("user32.dll", ReleaseDC, 2) { return 1; }
// (hWnd, lpPaint): PAINTSTRUCT hdc, fErase, rcPaint, fRestore, fIncUpdate,
// rgbReserved[32] (64 bytes). The update region is emptied; WM_ERASEBKGND
// is sent first when it asked for erasing (fErase then says whether the
// window procedure erased).
HLE("user32.dll", BeginPaint, 2) {
  const uint32_t hwnd = Arg(c, 0), ps = Arg(c, 1), dc = DcForWindow(hwnd);
  int r[4] = {0, 0, 0, 0};
  bool erase = false;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(hwnd)) {
      if (w->dirty) r[0] = w->ul, r[1] = w->ut, r[2] = w->ur, r[3] = w->ub;
      erase = w->dirty && w->erase;
      w->dirty = w->erase = false;
      w->paint_sent = false;
    }
  }
  bool erased = erase && Send(hwnd, WM_ERASEBKGND, dc, 0) != 0;
  std::memset(GPtr(ps), 0, 64);
  Wr32(ps, dc);
  Wr32(ps + 4, erase && !erased ? 1 : 0);
  for (int i = 0; i < 4; i++) Wr32(ps + 8 + 4 * i, uint32_t(r[i]));
  return dc;
}
HLE("user32.dll", EndPaint, 2) { return 1; }
HLE("user32.dll", FillRect, 3) { return 1; }
HLE("user32.dll", DrawTextA, 5) { return 16; }
HLE("user32.dll", DrawFrameControl, 4) { return 1; }

// ---- clipboard (not wired to the host yet) --------------------------------------------

HLE("user32.dll", OpenClipboard, 1) { return 0; }
HLE("user32.dll", CloseClipboard, 0) { return 1; }
HLE("user32.dll", EmptyClipboard, 0) { return 0; }
HLE("user32.dll", GetClipboardData, 1) { return 0; }
HLE("user32.dll", SetClipboardData, 2) { return 0; }

}  // namespace
}  // namespace hle
}  // namespace recomp
