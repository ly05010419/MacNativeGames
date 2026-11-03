#pragma once
// user32's window manager state, shared by the user32 sources:
// user32_window.cpp (classes, windows, queues, the SDL event pump),
// user32_dialog.cpp (dialog boxes) and user32_controls.cpp (the system
// control classes). Everything here is guarded by g_mu.

#include <SDL3/SDL.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace recomp {
struct Ctx;
// The desktop size in pixels (user32_window.cpp).
void DesktopSize(int* w, int* h);
}

namespace recomp::user32 {

// ---- messages and styles -----------------------------------------------------
enum : uint32_t {
  WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003,
  WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007,
  WM_KILLFOCUS = 0x0008, WM_ENABLE = 0x000A, WM_SETREDRAW = 0x000B,
  WM_SETTEXT = 0x000C, WM_GETTEXT = 0x000D,
  WM_GETTEXTLENGTH = 0x000E, WM_PAINT = 0x000F, WM_CLOSE = 0x0010,
  WM_QUIT = 0x0012, WM_ERASEBKGND = 0x0014, WM_SHOWWINDOW = 0x0018,
  WM_SETCURSOR = 0x0020, WM_MOUSEACTIVATE = 0x0021,
  WM_DRAWITEM = 0x002B, WM_MEASUREITEM = 0x002C, WM_DELETEITEM = 0x002D,
  WM_SETFONT = 0x0030, WM_GETFONT = 0x0031, WM_TIMER = 0x0113,
  WM_ACTIVATEAPP = 0x001C, WM_NEXTDLGCTL = 0x0028, WM_WINDOWPOSCHANGING = 0x0046,
  WM_WINDOWPOSCHANGED = 0x0047, WM_NOTIFY = 0x004E,
  WM_NCCREATE = 0x0081, WM_NCDESTROY = 0x0082,
  WM_NCHITTEST = 0x0084, WM_GETDLGCODE = 0x0087,
  WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102,
  WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105, WM_SYSCHAR = 0x0106,
  WM_INITDIALOG = 0x0110, WM_COMMAND = 0x0111, WM_HSCROLL = 0x0114, WM_VSCROLL = 0x0115,
  WM_SYSCOMMAND = 0x0112, WM_CTLCOLORMSGBOX = 0x0132, WM_CTLCOLOREDIT = 0x0133,
  WM_CTLCOLORLISTBOX = 0x0134, WM_CTLCOLORBTN = 0x0135, WM_CTLCOLORDLG = 0x0136,
  WM_CTLCOLORSCROLLBAR = 0x0137, WM_CTLCOLORSTATIC = 0x0138,
  WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201,
  WM_LBUTTONUP = 0x0202, WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONDOWN = 0x0204,
  WM_RBUTTONUP = 0x0205, WM_RBUTTONDBLCLK = 0x0206, WM_MBUTTONDOWN = 0x0207,
  WM_MBUTTONUP = 0x0208, WM_MBUTTONDBLCLK = 0x0209, WM_MOUSEWHEEL = 0x020A,
  WM_CAPTURECHANGED = 0x0215,
};
constexpr uint32_t WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000,
                   WS_DISABLED = 0x08000000, WS_CAPTION = 0x00C00000,
                   WS_TABSTOP = 0x00010000, WS_GROUP = 0x00020000,
                   WS_THICKFRAME = 0x00040000, WS_POPUP = 0x80000000;
constexpr uint32_t CS_DBLCLKS = 0x0008;
constexpr int32_t GWL_WNDPROC = -4, GWL_HINSTANCE = -6, GWL_HWNDPARENT = -8,
                  GWL_STYLE = -16, GWL_EXSTYLE = -20, GWL_USERDATA = -21,
                  GWL_ID = -12;
// Dialog window longs (DWL_MSGRESULT 0, DWL_DLGPROC 4, DWL_USER 8).
constexpr int32_t DWL_MSGRESULT = 0, DWL_DLGPROC = 4, DWL_USER = 8;

struct WndClass {
  std::string name;     // lower case (the g_classes key)
  std::string display;  // as registered (GetClassName): "Button", "MyClass"
  uint32_t style = 0, wndproc = 0, wnd_extra = 0;
  uint16_t atom = 0;
};

// An item of a list box or combo box.
struct ListItem {
  std::string text;
  uint32_t data = 0;      // LB_SETITEMDATA / CB_SETITEMDATA
  bool selected = false;  // multiple-selection list boxes
};

struct Window {
  uint32_t hwnd = 0;
  std::string cls;
  uint32_t wndproc = 0, style = 0, exstyle = 0, parent = 0, id = 0;
  uint32_t hinst = 0, userdata = 0, class_style = 0;
  // The window rect: in screen coordinates for a top-level window, in
  // its parent's client coordinates for a child (WS_CHILD), as Windows
  // keeps it.
  int x = 0, y = 0, w = 0, h = 0;
  std::string title;
  std::vector<uint8_t> extra;      // cbWndExtra bytes (GetWindowLong >= 0)
  uint32_t tid = 0;
  bool visible = false, enabled = true, destroyed = false;
  bool minimized = false;      // as the game asked (IsIconic)
  int mode_w = 0, mode_h = 0;  // DirectDraw exclusive mode, see header
  // Host fullscreen (UpdateFullscreen), entered for a screen of fs_w x fs_h.
  bool fullscreen = false;
  int fs_w = 0, fs_h = 0;
  SDL_Window* sdl = nullptr;
  // Place among its siblings: lower is nearer the top of the Z order. A
  // new child goes to the bottom (dialog controls keep template order).
  int64_t z = 0;
  // The update region, as a rectangle in client coordinates; WM_PAINT is
  // generated while it is not empty (dirty).
  bool dirty = false, erase = false;
  int ul = 0, ut = 0, ur = 0, ub = 0;
  // A WM_PAINT sent by UpdateWindow / RedrawWindow is still unprocessed
  // (no BeginPaint or ValidateRect yet): see PaintNow.
  bool paint_sent = false;
  bool redraw = true;  // WM_SETREDRAW
  // Dialog boxes (user32_dialog.cpp): the dialog procedure and the window
  // longs DefDlgProc keeps; a modal dialog's end state.
  bool dialog = false;
  uint32_t dlgproc = 0, msgresult = 0, dlguser = 0, def_id = 1;
  bool ended = false;
  uint32_t end_result = 0;
  // Built-in controls (user32_controls.cpp): the text is `title`; the
  // rest of their state is kept here.
  uint32_t sel_lo = 0, sel_hi = 0, font = 0, image = 0, check = 0;
  uint32_t limit = 30000;  // edit text limit (EM_SETLIMITTEXT)
  uint32_t bstate = 0;     // button BST_* state bits (pushed, focus)
  std::vector<ListItem> items;  // list box / combo box
  int cur = -1, top = 0, anchor = -1;  // selection, first visible item
  int item_h = 0;                      // fixed item height
  bool dropped = false;                // combo box list shown
  uint32_t popup = 0;                  // combo box: its list window
  int range_lo = 0, range_hi = 100, pos = 0, page = 0, line = 1;  // trackbar
};

struct Msg {
  uint32_t hwnd, msg, wparam, lparam, time;
  int32_t ptx, pty;
};

struct Queue {
  std::deque<Msg> msgs;
  bool quit = false;
  uint32_t quit_code = 0;
};

// SetTimer: WM_TIMER is not queued but synthesised when the owning
// thread's queue has nothing else for the caller, as on Windows.
struct Timer {
  uint32_t hwnd, id, elapse, proc, tid;
  int64_t due;  // TickCount() of the next WM_TIMER
};

// ClipCursor: while the game confines the cursor to one of our windows
// (mouse look), the cursor is virtual, as in exclusive mode.
struct Clip {
  bool on = false;
  int32_t l = 0, t = 0, r = 0, b = 0;  // screen coordinates
  uint32_t hwnd = 0;
};

// RegisterHotKey: a matching key press becomes WM_HOTKEY instead of a key
// message, as on Windows (here only while one of our windows has focus).
struct HotKey {
  uint32_t hwnd, id, mods, vk, tid;
};

extern std::recursive_mutex g_mu;
extern std::map<std::string, WndClass> g_classes;
extern std::map<uint32_t, Window> g_windows;
extern std::map<uint32_t, Queue> g_queues;  // by tid
extern std::vector<Timer> g_timers;
extern Clip g_clip;
extern uint32_t g_capture;
extern std::vector<HotKey> g_hotkeys;
extern std::condition_variable_any g_queue_cv;
extern uint32_t g_next_hwnd;
extern uint16_t g_next_atom;
extern uint32_t g_focus, g_active;
extern int32_t g_mouse_x, g_mouse_y;  // screen coordinates

// The live window behind hwnd, or nullptr.
Window* Win(uint32_t hwnd);
// Frame sizes for a top-level style: border on each side, caption on top.
void FrameSize(uint32_t style, int* border, int* caption);
// Client-area size of a window rect (children have no frame here).
void ClientSize(const Window& w, int* cw, int* ch);
// Screen position of hwnd's client area (0, 0).
void WindowOrigin(uint32_t hwnd, int* x, int* y);
uint32_t MkLparam(int lo, int hi);

void PostTo(uint32_t tid, const Msg& m);
void Post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
// SendMessage: the window procedure, called on the calling thread.
uint32_t Send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

// The children of `parent`, top of the Z order first (live ones only).
std::vector<uint32_t> Children(uint32_t parent);
// The deepest visible child of hwnd (or hwnd itself) at screen point
// (sx, sy), as WindowFromPoint resolves it within a top-level window.
uint32_t ChildAtScreen(uint32_t hwnd, int sx, int sy);
// The top-level window hwnd belongs to.
uint32_t TopLevel(uint32_t hwnd);
// Puts child hwnd at the top of its siblings' Z order.
void RaiseChild(uint32_t hwnd);
// Whether `ancestor` is hwnd or one of its parents.
bool IsSelfOrAncestor(uint32_t ancestor, uint32_t hwnd);

// Adds rect (client coordinates; null for all) to hwnd's update region.
void Invalidate(uint32_t hwnd, const int* rect, bool erase);
// Invalidates hwnd and every visible descendant (a window shown or moved).
void InvalidateTree(uint32_t hwnd);
// Sends WM_PAINT to hwnd now if its update region is not empty.
void PaintNow(uint32_t hwnd);

// Creates a window as CreateWindowExA does (used by dialogs). cls and
// title are host strings; `menu` is the child id for WS_CHILD.
uint32_t CreateWindowHost(uint32_t exstyle, const std::string& cls, const std::string& title,
                          uint32_t style, int x, int y, int w, int h, uint32_t parent,
                          uint32_t menu, uint32_t hinst, uint32_t param);
// Makes the system control and dialog classes exist.
void EnsureSystemClasses();
// The procedure behind a guest thunk name in user32 ("<button>", ...).
uint32_t ClassProc(const std::string& thunk_name);
// Calls DefWindowProcA for a control procedure's unhandled messages.
uint32_t CallDefWindowProc(Ctx& c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

}  // namespace recomp::user32
