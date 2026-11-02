// user32's system control classes: static, button, edit, listbox,
// combobox (with its drop-down list, "ComboLBox") and scrollbar, and the
// comctl32 trackbar and progress bar.
//
// Controls keep the state and send the notifications Windows controls do
// (WM_COMMAND, WM_HSCROLL, WM_DRAWITEM / WM_MEASUREITEM / WM_DELETEITEM
// for owner-drawn ones); nothing is drawn by the host. Games that use
// these controls draw them, owner-drawn or by subclassing them, and chain
// to these procedures (CallWindowProc) for their behaviour.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/input_state.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_internal.h"
#include "runtime/hle/user32_window.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

using namespace user32;
using namespace win32;

// ---- shared ---------------------------------------------------------------------------

constexpr uint32_t ODT_LISTBOX = 2, ODT_COMBOBOX = 3, ODT_BUTTON = 4, ODT_STATIC = 5;
constexpr uint32_t ODA_DRAWENTIRE = 1, ODA_SELECT = 2, ODA_FOCUS = 4;
constexpr uint32_t ODS_SELECTED = 0x1, ODS_DISABLED = 0x4, ODS_CHECKED = 0x8,
                   ODS_FOCUS = 0x10, ODS_DEFAULT = 0x20, ODS_COMBOBOXEDIT = 0x1000;
constexpr uint32_t DLGC_WANTARROWS = 0x1, DLGC_HASSETSEL = 0x8, DLGC_DEFPUSHBUTTON = 0x10,
                   DLGC_UNDEFPUSHBUTTON = 0x20, DLGC_RADIOBUTTON = 0x40,
                   DLGC_WANTCHARS = 0x80, DLGC_STATIC = 0x100, DLGC_BUTTON = 0x2000;
constexpr uint32_t LB_ERR = 0xFFFFFFFF;
constexpr int kTextHeight = 13;  // the 8-point dialog font's line height

uint32_t Parent(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  return w ? w->parent : 0;
}

uint32_t CtlId(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  return w ? w->id : 0;
}

// WM_COMMAND(MAKEWPARAM(id, code), hwnd) to the parent.
void Notify(uint32_t hwnd, uint32_t code) {
  if (uint32_t p = Parent(hwnd)) Send(p, WM_COMMAND, (CtlId(hwnd) & 0xFFFF) | code << 16, hwnd);
}

// WM_DRAWITEM to the parent with a DRAWITEMSTRUCT (CtlType, CtlID, itemID,
// itemAction, itemState, hwndItem, hDC, rcItem, itemData; 48 bytes).
// `owner`: the control whose parent draws (a combo box for its list);
// `on`: the window whose DC and client coordinates rcItem are in.
void DrawItem(uint32_t owner, uint32_t on, uint32_t type, uint32_t item, uint32_t action,
              uint32_t state, const int rc[4], uint32_t data) {
  const uint32_t parent = Parent(owner);
  if (!parent) return;
  const gaddr d = HeapAllocProcess(48, true);
  Wr32(d + 0, type);
  Wr32(d + 4, CtlId(owner));
  Wr32(d + 8, item);
  Wr32(d + 12, action);
  Wr32(d + 16, state);
  Wr32(d + 20, owner);
  Wr32(d + 24, DcForWindow(on));
  for (int i = 0; i < 4; i++) Wr32(d + 28 + 4 * i, uint32_t(rc[i]));
  Wr32(d + 44, data);
  RECOMP_TRACE("  WM_DRAWITEM(%X) type %u id %u item %d state %X rc (%d,%d,%d,%d) data %X\n",
               parent, type, CtlId(owner), int(item), state, rc[0], rc[1], rc[2], rc[3], data);
  Send(parent, WM_DRAWITEM, CtlId(owner), d);
  HeapFreeProcess(d);
}

// WM_MEASUREITEM to the parent (MEASUREITEMSTRUCT: CtlType, CtlID, itemID,
// itemWidth, itemHeight, itemData); the height it leaves.
int MeasureItem(uint32_t owner, uint32_t type, uint32_t item, int width, int height) {
  const uint32_t parent = Parent(owner);
  if (!parent) return height;
  const gaddr m = HeapAllocProcess(24, true);
  Wr32(m + 0, type);
  Wr32(m + 4, CtlId(owner));
  Wr32(m + 8, item);
  Wr32(m + 12, uint32_t(width));
  Wr32(m + 16, uint32_t(height));
  Send(parent, WM_MEASUREITEM, CtlId(owner), m);
  const int h = int(Rd32(m + 16));
  HeapFreeProcess(m);
  return h > 0 ? h : height;
}

void ClientRect(uint32_t hwnd, int rc[4]) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  rc[0] = rc[1] = 0;
  Window* w = Win(hwnd);
  int cw = 0, ch = 0;
  if (w) ClientSize(*w, &cw, &ch);
  rc[2] = cw;
  rc[3] = ch;
}

// The update region is consumed (a control's WM_PAINT).
void Validate(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd)) w->dirty = w->erase = false;
}

void Capture(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  g_capture = hwnd;
}

void ReleaseCaptureOf(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (g_capture == hwnd) g_capture = 0;
}

bool HasCapture(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_capture == hwnd;
}

bool HasFocus(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_focus == hwnd;
}

void Focus(Ctx& c, uint32_t hwnd) {
  if (HasFocus(hwnd)) return;
  CallGuest(c, ThunkFor("user32.dll", "SetFocus"), {hwnd});
}

int LoWord(uint32_t v) { return int16_t(v & 0xFFFF); }
int HiWord(uint32_t v) { return int16_t(v >> 16); }

// Messages every control handles the same way; false if `msg` is not one.
bool CommonControlMsg(Window& w, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t* r) {
  switch (msg) {
    case WM_NCCREATE: *r = 1; return true;
    case WM_SETTEXT: w.title = lp ? GStr(lp) : ""; *r = 1; return true;
    case WM_GETTEXT: {
      if (!wp) { *r = 0; return true; }
      std::string t = w.title.substr(0, wp - 1);
      PutStr(lp, t);
      *r = uint32_t(t.size());
      return true;
    }
    case WM_GETTEXTLENGTH: *r = uint32_t(w.title.size()); return true;
    case WM_SETFONT: {
      w.font = wp;
      // Where the class keeps it among its window extra bytes.
      const size_t off = w.cls == "button" ? 4 : w.cls == "static" ? 0 : SIZE_MAX;
      if (off != SIZE_MAX && w.extra.size() >= off + 4) std::memcpy(w.extra.data() + off, &wp, 4);
      if (lp) Invalidate(w.hwnd, nullptr, true);
      *r = 0;
      return true;
    }
    case WM_GETFONT: *r = w.font; return true;
    default: return false;
  }
}

// Unknown control messages are reported once each and return 0.
void UnhandledControlMsg(const char* cls, uint32_t msg) {
  static std::set<std::pair<std::string, uint32_t>> seen;
  if (seen.insert({cls, msg}).second)
    WarnF("%s control: message %04X not modelled, returning 0\n", cls, msg);
}

// ---- static -----------------------------------------------------------------------------

constexpr uint32_t SS_TYPEMASK = 0x1F, SS_OWNERDRAW = 0x0D, SS_NOTIFY = 0x100;
constexpr uint32_t STM_SETICON = 0x0170, STM_GETICON = 0x0171, STM_SETIMAGE = 0x0172,
                   STM_GETIMAGE = 0x0173;

HLE_NAMED("user32.dll", "<static>", StaticWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  uint32_t style;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) {
      if (msg == WM_SETTEXT) Invalidate(hwnd, nullptr, true);
      return r;
    }
    style = w->style;
    switch (msg) {
      case STM_SETIMAGE: case STM_SETICON:
        r = w->image;
        w->image = msg == STM_SETICON ? wp : lp;
        Invalidate(hwnd, nullptr, true);
        return r;
      case STM_GETIMAGE: case STM_GETICON: return w->image;
    }
    if (msg >= 0x0170 && msg <= 0x017F) {
      UnhandledControlMsg("static", msg);
      return 0;
    }
  }
  switch (msg) {
    case WM_CREATE: return 0;
    case WM_PAINT: {
      Validate(hwnd);
      if ((style & SS_TYPEMASK) == SS_OWNERDRAW) {
        int rc[4];
        ClientRect(hwnd, rc);
        DrawItem(hwnd, hwnd, ODT_STATIC, 0, ODA_DRAWENTIRE, 0, rc, 0);
      }
      return 0;
    }
    case WM_LBUTTONDOWN:
      if (style & SS_NOTIFY) Notify(hwnd, 0);  // STN_CLICKED
      return 0;
    case WM_LBUTTONDBLCLK:
      if (style & SS_NOTIFY) Notify(hwnd, 1);  // STN_DBLCLK
      return 0;
    case WM_ENABLE:
      if (style & SS_NOTIFY) Notify(hwnd, wp ? 2 : 3);  // STN_ENABLE / STN_DISABLE
      Invalidate(hwnd, nullptr, true);
      return 0;
    case WM_GETDLGCODE: return DLGC_STATIC;
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

// ---- button -----------------------------------------------------------------------------

constexpr uint32_t BS_TYPEMASK = 0xF, BS_PUSHBUTTON = 0, BS_DEFPUSHBUTTON = 1,
                   BS_CHECKBOX = 2, BS_AUTOCHECKBOX = 3, BS_RADIOBUTTON = 4, BS_3STATE = 5,
                   BS_AUTO3STATE = 6, BS_GROUPBOX = 7, BS_AUTORADIOBUTTON = 9,
                   BS_OWNERDRAW = 0xB, BS_NOTIFY = 0x4000;
constexpr uint32_t BST_CHECKMASK = 0x3, BST_PUSHED = 0x4, BST_FOCUS = 0x8;
constexpr uint32_t BM_GETCHECK = 0x00F0, BM_SETCHECK = 0x00F1, BM_GETSTATE = 0x00F2,
                   BM_SETSTATE = 0x00F3, BM_SETSTYLE = 0x00F4, BM_CLICK = 0x00F5,
                   BM_GETIMAGE = 0x00F6, BM_SETIMAGE = 0x00F7;
constexpr uint32_t BN_CLICKED = 0, BN_DOUBLECLICKED = 5, BN_SETFOCUS = 6, BN_KILLFOCUS = 7;

uint32_t ButtonType(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  return w ? w->style & BS_TYPEMASK : 0;
}

// Redraws an owner-drawn button for `action` (ODA_*); other buttons get
// their update region (the game draws them, subclassing).
void ButtonRedraw(uint32_t hwnd, uint32_t action) {
  uint32_t type, state, enabled;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return;
    type = w->style & BS_TYPEMASK;
    state = w->bstate | w->check;
    enabled = w->enabled;
  }
  if (type != BS_OWNERDRAW) {
    Invalidate(hwnd, nullptr, false);
    return;
  }
  uint32_t ods = 0;
  if (state & BST_PUSHED) ods |= ODS_SELECTED;
  if (state & BST_FOCUS) ods |= ODS_FOCUS;
  if (!enabled) ods |= ODS_DISABLED;
  int rc[4];
  ClientRect(hwnd, rc);
  DrawItem(hwnd, hwnd, ODT_BUTTON, 0, action, ods, rc, 0);
}

void SetButtonState(uint32_t hwnd, uint32_t mask, bool on, uint32_t action) {
  bool changed;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return;
    const uint32_t old = w->bstate;
    w->bstate = on ? (w->bstate | mask) : (w->bstate & ~mask);
    changed = old != w->bstate;
  }
  if (changed) ButtonRedraw(hwnd, action);
}

// The radio buttons of hwnd's group (between WS_GROUP siblings).
std::vector<uint32_t> RadioGroup(uint32_t hwnd) {
  std::vector<uint32_t> sibs = Children(Parent(hwnd)), group;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = std::find(sibs.begin(), sibs.end(), hwnd);
  if (it == sibs.end()) return {hwnd};
  auto first = it;
  while (first != sibs.begin()) {
    Window* w = Win(*first);
    if (w && (w->style & WS_GROUP)) break;
    --first;
  }
  for (auto k = first; k != sibs.end(); ++k) {
    Window* w = Win(*k);
    if (k != first && w && (w->style & WS_GROUP)) break;
    if (w && w->cls == "button" &&
        ((w->style & BS_TYPEMASK) == BS_RADIOBUTTON ||
         (w->style & BS_TYPEMASK) == BS_AUTORADIOBUTTON))
      group.push_back(*k);
  }
  return group;
}

void SetCheck(uint32_t hwnd, uint32_t check) {
  bool changed;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return;
    changed = w->check != (check & BST_CHECKMASK);
    w->check = check & BST_CHECKMASK;
  }
  if (changed) ButtonRedraw(hwnd, ODA_SELECT);
}

// A click (mouse release or space): the automatic types change their
// check first, then the parent gets BN_CLICKED.
void ButtonClick(uint32_t hwnd) {
  const uint32_t type = ButtonType(hwnd);
  uint32_t check;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    check = w ? w->check : 0;
  }
  if (type == BS_AUTOCHECKBOX) SetCheck(hwnd, check ? 0 : 1);
  if (type == BS_AUTO3STATE) SetCheck(hwnd, (check + 1) % 3);
  if (type == BS_AUTORADIOBUTTON)
    for (uint32_t k : RadioGroup(hwnd)) SetCheck(k, k == hwnd ? 1 : 0);
  Notify(hwnd, BN_CLICKED);
}

bool PointInClient(uint32_t hwnd, uint32_t lp) {
  int rc[4];
  ClientRect(hwnd, rc);
  const int x = LoWord(lp), y = HiWord(lp);
  return x >= rc[0] && y >= rc[1] && x < rc[2] && y < rc[3];
}

HLE_NAMED("user32.dll", "<button>", ButtonWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  uint32_t style;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) {
      if (msg == WM_SETTEXT) Invalidate(hwnd, nullptr, true);
      return r;
    }
    style = w->style;
  }
  const uint32_t type = style & BS_TYPEMASK;
  switch (msg) {
    case WM_CREATE: return 0;
    case WM_PAINT:
      Validate(hwnd);
      if (type == BS_OWNERDRAW) ButtonRedraw(hwnd, ODA_DRAWENTIRE);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      if (type == BS_GROUPBOX) return 0;
      if (msg == WM_LBUTTONDBLCLK && (style & BS_NOTIFY)) Notify(hwnd, BN_DOUBLECLICKED);
      Capture(hwnd);
      Focus(c, hwnd);
      SetButtonState(hwnd, BST_PUSHED, true, ODA_SELECT);
      return 0;
    case WM_MOUSEMOVE:
      if (HasCapture(hwnd)) SetButtonState(hwnd, BST_PUSHED, PointInClient(hwnd, lp), ODA_SELECT);
      return 0;
    case WM_LBUTTONUP: {
      if (!HasCapture(hwnd)) return 0;
      ReleaseCaptureOf(hwnd);
      const bool inside = PointInClient(hwnd, lp);
      SetButtonState(hwnd, BST_PUSHED, false, ODA_SELECT);
      if (inside) ButtonClick(hwnd);
      return 0;
    }
    case WM_KEYDOWN:
      if (wp == 0x20) SetButtonState(hwnd, BST_PUSHED, true, ODA_SELECT);  // VK_SPACE
      return 0;
    case WM_KEYUP:
      if (wp == 0x20) {
        SetButtonState(hwnd, BST_PUSHED, false, ODA_SELECT);
        ButtonClick(hwnd);
      }
      return 0;
    case WM_SETFOCUS:
      SetButtonState(hwnd, BST_FOCUS, true, ODA_FOCUS);
      if (style & BS_NOTIFY) Notify(hwnd, BN_SETFOCUS);
      return 0;
    case WM_KILLFOCUS:
      if (HasCapture(hwnd)) ReleaseCaptureOf(hwnd);
      SetButtonState(hwnd, BST_FOCUS | BST_PUSHED, false, ODA_FOCUS);
      if (style & BS_NOTIFY) Notify(hwnd, BN_KILLFOCUS);
      return 0;
    case WM_ENABLE:
      ButtonRedraw(hwnd, ODA_DRAWENTIRE);
      return 0;
    case WM_GETDLGCODE:
      if (type == BS_GROUPBOX) return DLGC_STATIC;
      if (type == BS_DEFPUSHBUTTON) return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
      if (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON)
        return DLGC_BUTTON | DLGC_RADIOBUTTON;
      return DLGC_BUTTON | (type == BS_PUSHBUTTON ? DLGC_UNDEFPUSHBUTTON : 0);
    case BM_GETCHECK: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      return w ? w->check : 0;
    }
    case BM_SETCHECK:
      SetCheck(hwnd, wp);
      return 0;
    case BM_GETSTATE: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      return w ? w->check | w->bstate : 0;
    }
    case BM_SETSTATE:
      SetButtonState(hwnd, BST_PUSHED, wp != 0, ODA_SELECT);
      return 0;
    case BM_SETSTYLE: {
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        if (Window* w = Win(hwnd)) w->style = (w->style & ~0xFFFFu) | (wp & 0xFFFF);
      }
      if (lp) ButtonRedraw(hwnd, ODA_DRAWENTIRE);
      return 0;
    }
    case BM_CLICK:
      SetButtonState(hwnd, BST_PUSHED, true, ODA_SELECT);
      SetButtonState(hwnd, BST_PUSHED, false, ODA_SELECT);
      ButtonClick(hwnd);
      return 0;
    case BM_GETIMAGE: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      return w ? w->image : 0;
    }
    case BM_SETIMAGE: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      if (!w) return 0;
      r = w->image;
      w->image = lp;
      return r;
    }
  }
  if (msg >= 0x00F0 && msg <= 0x00FF) {
    UnhandledControlMsg("button", msg);
    return 0;
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

// ---- edit -------------------------------------------------------------------------------
//
// The text is the window title. A read-only multi-line edit is a log the
// game appends to (a dedicated console window): appended text is echoed
// to stdout, which is where that log is visible here.

constexpr uint32_t ES_MULTILINE = 0x0004, ES_READONLY = 0x0800, ES_NUMBER = 0x2000;
constexpr uint32_t EM_GETSEL = 0x00B0, EM_SETSEL = 0x00B1, EM_LINESCROLL = 0x00B6,
                   EM_SCROLLCARET = 0x00B7, EM_GETLINECOUNT = 0x00BA,
                   EM_REPLACESEL = 0x00C2, EM_SETLIMITTEXT = 0x00C5,
                   EM_SETREADONLY = 0x00CF;
constexpr uint32_t EN_SETFOCUS = 0x0100, EN_KILLFOCUS = 0x0200, EN_CHANGE = 0x0300,
                   EN_UPDATE = 0x0400, EN_MAXTEXT = 0x0501;

// Writes s to stdout with CRLF and lone CR as plain newlines.
void EchoLog(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\r') {
      if (i + 1 < s.size() && s[i + 1] == '\n') continue;
      out += '\n';
    } else {
      out += s[i];
    }
  }
  std::fwrite(out.data(), 1, out.size(), stdout);
  std::fflush(stdout);
}

HLE_NAMED("user32.dll", "<edit>", EditWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  std::string echo;
  bool changed = false, full = false;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (msg == WM_SETTEXT) w->sel_lo = w->sel_hi = 0;
    if (CommonControlMsg(*w, msg, wp, lp, &r)) {
      if (msg == WM_SETTEXT) Invalidate(hwnd, nullptr, true);
      return r;
    }
    const uint32_t len = uint32_t(w->title.size());
    w->sel_lo = std::min(w->sel_lo, len);
    w->sel_hi = std::min(w->sel_hi, len);
    // Replaces the selection with t (cut to the text limit), caret after it.
    auto replace = [&](const std::string& t) {
      uint32_t room = w->limit - std::min(w->limit, len - (w->sel_hi - w->sel_lo));
      std::string ins = t.substr(0, room);
      full = ins.size() < t.size();
      w->title.replace(w->sel_lo, w->sel_hi - w->sel_lo, ins);
      w->sel_lo = w->sel_hi = w->sel_lo + uint32_t(ins.size());
      return ins;
    };
    switch (msg) {
      case WM_CREATE: return 0;
      case EM_GETSEL:
        if (wp) Wr32(wp, w->sel_lo);
        if (lp) Wr32(lp, w->sel_hi);
        return MkLparam(int(std::min(w->sel_lo, 0xFFFFu)), int(std::min(w->sel_hi, 0xFFFFu)));
      case EM_SETSEL: {
        int32_t a = int32_t(wp), b = int32_t(lp);
        if (a == -1) {  // remove the selection, keep the caret
          w->sel_lo = w->sel_hi;
          return 1;
        }
        uint32_t ua = a < 0 ? len : std::min(uint32_t(a), len);
        uint32_t ub = b < 0 ? len : std::min(uint32_t(b), len);
        w->sel_lo = std::min(ua, ub);
        w->sel_hi = std::max(ua, ub);
        return 1;
      }
      case EM_REPLACESEL: {
        std::string ins = replace(lp ? GStr(lp) : "");
        if ((w->style & (ES_MULTILINE | ES_READONLY)) == (ES_MULTILINE | ES_READONLY))
          echo = ins;
        changed = true;
        break;
      }
      case EM_LINESCROLL: return (w->style & ES_MULTILINE) ? 1 : 0;
      case EM_SCROLLCARET: return 1;
      case EM_GETLINECOUNT:
        return 1 + uint32_t(std::count(w->title.begin(), w->title.end(), '\n'));
      case EM_SETLIMITTEXT:
        w->limit = wp ? wp : ((w->style & ES_MULTILINE) ? 0xFFFFFFFFu : 0x7FFFFFFEu);
        return 0;
      case EM_SETREADONLY:
        w->style = wp ? (w->style | ES_READONLY) : (w->style & ~ES_READONLY);
        return 1;
      case WM_CHAR:
        if (w->style & ES_READONLY) return 0;
        if (wp == 8) {  // backspace: the selection, or the character before
          if (w->sel_lo == w->sel_hi && w->sel_lo > 0) w->sel_lo--;
          replace("");
          changed = true;
        } else if (wp >= 32 && wp < 256) {
          if ((w->style & ES_NUMBER) && (wp < '0' || wp > '9')) return 0;
          replace(std::string(1, char(wp)));
          changed = true;
        }
        break;
      case WM_KEYDOWN:  // caret movement
        switch (wp) {
          case 0x25: if (w->sel_lo) w->sel_lo--; w->sel_hi = w->sel_lo; break;  // left
          case 0x27: if (w->sel_hi < len) w->sel_hi++; w->sel_lo = w->sel_hi; break;
          case 0x24: w->sel_lo = w->sel_hi = 0; break;    // home
          case 0x23: w->sel_lo = w->sel_hi = len; break;  // end
          case 0x2E:  // delete: the selection, or the character after
            if (w->style & ES_READONLY) break;
            if (w->sel_lo == w->sel_hi && w->sel_hi < len) w->sel_hi++;
            replace("");
            changed = true;
            break;
        }
        if (!changed) {
          Invalidate(hwnd, nullptr, false);
          return 0;
        }
        break;
      case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS;
      default:
        if (msg >= 0x00B0 && msg <= 0x00DF) {
          UnhandledControlMsg("edit", msg);
          return 0;
        }
        break;
    }
  }
  if (!echo.empty()) EchoLog(echo);
  if (changed) {
    Invalidate(hwnd, nullptr, true);
    if (full) Notify(hwnd, EN_MAXTEXT);
    Notify(hwnd, EN_UPDATE);
    Notify(hwnd, EN_CHANGE);
    return msg == EM_REPLACESEL || msg == WM_CHAR || msg == WM_KEYDOWN ? 0 : r;
  }
  switch (msg) {
    case WM_PAINT: Validate(hwnd); return 0;
    case WM_LBUTTONDOWN: Focus(c, hwnd); return 0;
    case WM_SETFOCUS: Invalidate(hwnd, nullptr, false); Notify(hwnd, EN_SETFOCUS); return 0;
    case WM_KILLFOCUS: Invalidate(hwnd, nullptr, false); Notify(hwnd, EN_KILLFOCUS); return 0;
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

// ---- list box ---------------------------------------------------------------------------

constexpr uint32_t LBS_NOTIFY = 0x1, LBS_SORT = 0x2, LBS_MULTIPLESEL = 0x8,
                   LBS_OWNERDRAWFIXED = 0x10, LBS_OWNERDRAWVARIABLE = 0x20,
                   LBS_HASSTRINGS = 0x40, LBS_EXTENDEDSEL = 0x800, LBS_NOSEL = 0x4000;
constexpr uint32_t LBN_SELCHANGE = 1, LBN_DBLCLK = 2, LBN_SETFOCUS = 4, LBN_KILLFOCUS = 5;

// A list's items, shared by the list box and the combo box (whose items
// the drop-down list shows).
struct List {
  uint32_t hwnd;  // the window whose Window::items / cur / top hold them
  bool strings;   // items carry strings (else the item is its data)
  bool sorted;
};

// Inserts an item at `at` (-1: the end, or the sorted place); its index.
int ListInsert(Window& w, const List& li, int at, uint32_t lp) {
  ListItem it;
  if (li.strings) {
    it.text = lp ? GStr(lp) : "";
  } else {
    it.data = lp;
  }
  int n = int(w.items.size());
  if (at < 0 || at > n) at = n;
  if (li.sorted && li.strings && at == n) {
    at = 0;
    const std::string key = Lower(it.text);
    while (at < n && Lower(w.items[at].text) <= key) at++;
  }
  w.items.insert(w.items.begin() + at, it);
  if (w.cur >= at) w.cur++;
  return at;
}

// LB_FINDSTRING / LB_FINDSTRINGEXACT (and CB_): the first item after
// `start` (wrapping) whose text starts with / equals s, case-insensitive.
int ListFind(const Window& w, int start, const std::string& s, bool exact) {
  const int n = int(w.items.size());
  const std::string key = Lower(s);
  for (int i = 1; i <= n; i++) {
    const int k = ((start < 0 ? -1 : start) + i) % n;
    const std::string t = Lower(w.items[k].text);
    if (exact ? t == key : t.compare(0, key.size(), key) == 0) return k;
  }
  return -1;
}

void SendDeleteItem(uint32_t owner, uint32_t type, uint32_t item, uint32_t data) {
  const uint32_t parent = Parent(owner);
  if (!parent) return;
  const gaddr d = HeapAllocProcess(20, true);  // DELETEITEMSTRUCT
  Wr32(d + 0, type);
  Wr32(d + 4, CtlId(owner));
  Wr32(d + 8, item);
  Wr32(d + 12, owner);
  Wr32(d + 16, data);
  Send(parent, WM_DELETEITEM, CtlId(owner), d);
  HeapFreeProcess(d);
}

int ItemHeight(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  return w && w->item_h > 0 ? w->item_h : kTextHeight;
}

int VisibleItems(uint32_t hwnd) {
  int rc[4];
  ClientRect(hwnd, rc);
  return std::max(1, (rc[3] - rc[1]) / ItemHeight(hwnd));
}

// Draws the visible items of a list (owner-drawn), `owner`'s parent doing
// it; `on` is the list window (a combo box's drop-down list draws the
// combo box's items).
void PaintList(uint32_t owner, uint32_t on, uint32_t type, uint32_t items_of) {
  std::vector<ListItem> items;
  int cur, top;
  bool focus;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(items_of);
    if (!w) return;
    items = w->items;
    cur = w->cur;
    top = w->top;
    focus = g_focus == on;
  }
  const int ih = ItemHeight(on), n = VisibleItems(on);
  int rc[4];
  ClientRect(on, rc);
  for (int i = top; i < int(items.size()) && i < top + n; i++) {
    const int item_rc[4] = {rc[0], (i - top) * ih, rc[2], (i - top + 1) * ih};
    uint32_t state = items[size_t(i)].selected || i == cur ? ODS_SELECTED : 0;
    if (focus && i == cur) state |= ODS_FOCUS;
    DrawItem(owner, on, type, uint32_t(i), ODA_DRAWENTIRE, state, item_rc,
             items[size_t(i)].data);
  }
}

bool IsOwnerDrawList(uint32_t style) {
  return style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE);
}

// Sets the current item of a list box (scrolled into view); true if it
// changed.
bool ListSetCur(uint32_t hwnd, int i) {
  const int n = VisibleItems(hwnd);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w || i < -1 || i >= int(w->items.size())) return false;
  const bool changed = w->cur != i;
  w->cur = i;
  if (i >= 0 && i < w->top) w->top = i;
  if (i >= 0 && i >= w->top + n) w->top = i - n + 1;
  Invalidate(hwnd, nullptr, true);
  return changed;
}

void ListScrollTo(uint32_t hwnd, int top) {
  const int n = VisibleItems(hwnd);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w) return;
  top = std::clamp(top, 0, std::max(0, int(w->items.size()) - n));
  if (w->top != top) {
    w->top = top;
    Invalidate(hwnd, nullptr, true);
  }
}

// The list messages a list box and a combo box share (LB_x and CB_x);
// `lb` tells which numbering msg uses. Returns false if msg is not one.
bool ListMsg(Ctx& c, uint32_t hwnd, const List& li, uint32_t type, bool lb, uint32_t msg,
             uint32_t wp, uint32_t lp, uint32_t* r) {
  (void)c;
  // The two numberings: LB_ADDSTRING 0x180 ... / CB_ADDSTRING 0x143 ...
  enum Op { Add, Insert, Delete, Reset, Count, SetCur, GetCur, GetText, GetTextLen,
            GetData, SetData, Find, FindExact, Select, SetHeight, GetHeight, GetTop,
            SetTop, None };
  struct Map { uint32_t lb, cb; Op op; };
  static const Map kMap[] = {
      {0x180, 0x143, Add}, {0x181, 0x14A, Insert}, {0x182, 0x144, Delete},
      {0x184, 0x14B, Reset}, {0x18B, 0x146, Count}, {0x186, 0x14E, SetCur},
      {0x188, 0x147, GetCur}, {0x189, 0x148, GetText}, {0x18A, 0x149, GetTextLen},
      {0x199, 0x150, GetData}, {0x19A, 0x151, SetData}, {0x18F, 0x14C, Find},
      {0x1A2, 0x158, FindExact}, {0x18C, 0x14D, Select}, {0x1A0, 0x153, SetHeight},
      {0x1A1, 0x154, GetHeight}, {0x18E, 0x15B, GetTop}, {0x197, 0x15C, SetTop},
  };
  Op op = None;
  for (const Map& m : kMap)
    if ((lb ? m.lb : m.cb) == msg) op = m.op;
  if (op == None) return false;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w) {
    *r = LB_ERR;
    return true;
  }
  const int n = int(w->items.size());
  const int i = int(int32_t(wp));
  switch (op) {
    case Add: *r = uint32_t(ListInsert(*w, li, -1, lp)); break;
    case Insert: *r = uint32_t(ListInsert(*w, li, i, lp)); break;
    case Delete:
      if (i < 0 || i >= n) { *r = LB_ERR; break; }
      if (IsOwnerDrawList(w->style) || !li.strings)
        SendDeleteItem(hwnd, type, uint32_t(i), w->items[size_t(i)].data);
      w->items.erase(w->items.begin() + i);
      if (w->cur == i) w->cur = -1;
      else if (w->cur > i) w->cur--;
      *r = uint32_t(w->items.size());
      break;
    case Reset:
      if (IsOwnerDrawList(w->style) || !li.strings)
        for (int k = n - 1; k >= 0; k--)
          SendDeleteItem(hwnd, type, uint32_t(k), w->items[size_t(k)].data);
      w->items.clear();
      w->cur = -1;
      w->top = 0;
      *r = 0;
      break;
    case Count: *r = uint32_t(n); break;
    case SetCur:
      if (i < -1 || i >= n) {
        w->cur = -1;
        *r = LB_ERR;
      } else {
        w->cur = i;
        *r = i < 0 ? LB_ERR : uint32_t(i);
      }
      break;
    case GetCur: *r = w->cur < 0 ? LB_ERR : uint32_t(w->cur); break;
    case GetText:
      if (i < 0 || i >= n) { *r = LB_ERR; break; }
      if (li.strings) {
        PutStr(lp, w->items[size_t(i)].text);
        *r = uint32_t(w->items[size_t(i)].text.size());
      } else {
        Wr32(lp, w->items[size_t(i)].data);
        *r = 4;
      }
      break;
    case GetTextLen:
      *r = i < 0 || i >= n ? LB_ERR : li.strings ? uint32_t(w->items[size_t(i)].text.size()) : 4;
      break;
    case GetData: *r = i < 0 || i >= n ? LB_ERR : w->items[size_t(i)].data; break;
    case SetData:
      if (i < 0 || i >= n) { *r = LB_ERR; break; }
      w->items[size_t(i)].data = lp;
      *r = 1;
      break;
    case Find: case FindExact: case Select: {
      const int k = n ? ListFind(*w, i, lp ? GStr(lp) : "", op == FindExact) : -1;
      if (op == Select && k >= 0) w->cur = k;
      *r = k < 0 ? LB_ERR : uint32_t(k);
      break;
    }
    case SetHeight: w->item_h = int(lp & 0xFFFF); *r = 0; break;
    case GetHeight: *r = uint32_t(w->item_h > 0 ? w->item_h : kTextHeight); break;
    case GetTop: *r = uint32_t(w->top); break;
    case SetTop: w->top = std::clamp(i, 0, std::max(0, n - 1)); *r = 0; break;
    case None: break;
  }
  Invalidate(hwnd, nullptr, true);
  return true;
}

void ListCreate(uint32_t hwnd, uint32_t owner, uint32_t type) {
  uint32_t style;
  int rc[4];
  ClientRect(hwnd, rc);
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return;
    style = w->style;
  }
  if (IsOwnerDrawList(style)) {
    const int h = MeasureItem(owner, type, 0, rc[2], kTextHeight);
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(hwnd)) w->item_h = h;
  }
}

HLE_NAMED("user32.dll", "<listbox>", ListBoxWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  uint32_t style;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    style = w->style;
    if (msg != WM_SETTEXT && msg != WM_GETTEXT && CommonControlMsg(*w, msg, wp, lp, &r)) return r;
  }
  const List li{hwnd, !IsOwnerDrawList(style) || (style & LBS_HASSTRINGS),
                (style & LBS_SORT) != 0};
  if (ListMsg(c, hwnd, li, ODT_LISTBOX, true, msg, wp, lp, &r)) return r;
  const bool notify = style & LBS_NOTIFY;
  switch (msg) {
    case WM_CREATE:
      ListCreate(hwnd, hwnd, ODT_LISTBOX);
      return 0;
    case WM_PAINT:
      Validate(hwnd);
      if (IsOwnerDrawList(style)) PaintList(hwnd, hwnd, ODT_LISTBOX, hwnd);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      Focus(c, hwnd);
      if (style & LBS_NOSEL) return 0;
      int top, n;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(hwnd);
        top = w ? w->top : 0;
        n = w ? int(w->items.size()) : 0;
      }
      const int i = top + HiWord(lp) / ItemHeight(hwnd);
      if (i < 0 || i >= n) return 0;
      if (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        if (Window* w = Win(hwnd)) w->items[size_t(i)].selected = !w->items[size_t(i)].selected;
      }
      ListSetCur(hwnd, i);
      if (notify) Notify(hwnd, msg == WM_LBUTTONDBLCLK ? LBN_DBLCLK : LBN_SELCHANGE);
      return 0;
    }
    case WM_KEYDOWN: {
      int cur, n;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(hwnd);
        cur = w ? w->cur : -1;
        n = w ? int(w->items.size()) : 0;
      }
      const int page = VisibleItems(hwnd);
      int to = cur;
      switch (wp) {
        case 0x26: to = cur - 1; break;     // up
        case 0x28: to = cur + 1; break;     // down
        case 0x21: to = cur - page; break;  // page up
        case 0x22: to = cur + page; break;  // page down
        case 0x24: to = 0; break;           // home
        case 0x23: to = n - 1; break;       // end
        default: return 0;
      }
      to = std::clamp(to, 0, std::max(0, n - 1));
      if (n && !(style & LBS_NOSEL) && ListSetCur(hwnd, to) && notify)
        Notify(hwnd, LBN_SELCHANGE);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      const int top = w ? w->top : 0;
      ListScrollTo(hwnd, top - HiWord(wp) / 120 * 3);
      return 0;
    }
    case WM_VSCROLL: {
      int top, n;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(hwnd);
        top = w ? w->top : 0;
        n = w ? int(w->items.size()) : 0;
      }
      const int page = VisibleItems(hwnd);
      switch (wp & 0xFFFF) {
        case 0: top--; break;            // SB_LINEUP
        case 1: top++; break;            // SB_LINEDOWN
        case 2: top -= page; break;      // SB_PAGEUP
        case 3: top += page; break;      // SB_PAGEDOWN
        case 4: case 5: top = HiWord(wp); break;  // SB_THUMBPOSITION / TRACK
        case 6: top = 0; break;          // SB_TOP
        case 7: top = n; break;          // SB_BOTTOM
      }
      ListScrollTo(hwnd, top);
      return 0;
    }
    case WM_SETFOCUS:
      Invalidate(hwnd, nullptr, false);
      if (notify) Notify(hwnd, LBN_SETFOCUS);
      return 0;
    case WM_KILLFOCUS:
      Invalidate(hwnd, nullptr, false);
      if (notify) Notify(hwnd, LBN_KILLFOCUS);
      return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case 0x0198: {  // LB_GETITEMRECT(index, lpRect)
      int rc[4], top;
      ClientRect(hwnd, rc);
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(hwnd);
        top = w ? w->top : 0;
      }
      const int ih = ItemHeight(hwnd), i = int(int32_t(wp));
      const int out[4] = {rc[0], (i - top) * ih, rc[2], (i - top + 1) * ih};
      for (int k = 0; k < 4; k++) Wr32(lp + 4 * k, uint32_t(out[k]));
      return 1;
    }
    case 0x01A9: {  // LB_ITEMFROMPOINT(0, MAKELPARAM(x, y)): HIWORD 1 if outside
      int top, n;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(hwnd);
        top = w ? w->top : 0;
        n = w ? int(w->items.size()) : 0;
      }
      const int i = top + HiWord(lp) / ItemHeight(hwnd);
      const bool outside = !PointInClient(hwnd, lp) || i >= n;
      return uint32_t(std::clamp(i, 0, std::max(0, n - 1))) | (outside ? 0x10000u : 0);
    }
    case 0x0185: {  // LB_SETSEL(fSelect, index): -1 is every item
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      if (!w) return LB_ERR;
      const int i = int(int32_t(lp));
      for (int k = 0; k < int(w->items.size()); k++)
        if (i == -1 || k == i) w->items[size_t(k)].selected = wp != 0;
      Invalidate(hwnd, nullptr, true);
      return 0;
    }
    case 0x0187: {  // LB_GETSEL(index)
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      const int i = int(int32_t(wp));
      if (!w || i < 0 || i >= int(w->items.size())) return LB_ERR;
      return w->items[size_t(i)].selected || i == w->cur ? 1 : 0;
    }
    case 0x0190: {  // LB_GETSELCOUNT
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      if (!w || !(w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL))) return LB_ERR;
      return uint32_t(std::count_if(w->items.begin(), w->items.end(),
                                    [](const ListItem& it) { return it.selected; }));
    }
    case 0x019E: return ListSetCur(hwnd, int(int32_t(wp))) ? 0 : 0;  // LB_SETCARETINDEX
    case 0x019F: {  // LB_GETCARETINDEX
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      return w ? uint32_t(std::max(w->cur, 0)) : 0;
    }
    case 0x0192: return 1;  // LB_SETTABSTOPS
    case 0x0193: case 0x0194: case 0x0195: return 0;  // horizontal extent, column width
    case 0x01A8: return 0;  // LB_INITSTORAGE
  }
  if (msg >= 0x0180 && msg <= 0x01AF) {
    UnhandledControlMsg("listbox", msg);
    return 0;
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

// ---- combo box ---------------------------------------------------------------------------
//
// Drop-down list combo boxes: the selection field is the combo box window
// (its height is the field's, as Windows sizes it at creation); the list
// appears below it in a "ComboLBox" window while dropped. That list is a
// child of the combo box's top-level window here, at the place the
// drop-down list would appear on screen, so it is hit-tested and drawn
// (WM_DRAWITEM to the combo box's parent) like any child.

constexpr uint32_t CBS_TYPEMASK = 0x3, CBS_SIMPLE = 1, CBS_OWNERDRAWFIXED = 0x10,
                   CBS_OWNERDRAWVARIABLE = 0x20, CBS_SORT = 0x100, CBS_HASSTRINGS = 0x200;
constexpr uint32_t CBN_SELCHANGE = 1, CBN_SETFOCUS = 3, CBN_KILLFOCUS = 4, CBN_DROPDOWN = 7,
                   CBN_CLOSEUP = 8, CBN_SELENDOK = 9, CBN_SELENDCANCEL = 10;

bool IsOwnerDrawCombo(uint32_t style) {
  return style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE);
}

void ComboPaintField(uint32_t combo) {
  uint32_t style, data = 0;
  int cur;
  bool focus;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(combo);
    if (!w) return;
    style = w->style;
    cur = w->cur;
    if (cur >= 0 && cur < int(w->items.size())) data = w->items[size_t(cur)].data;
    focus = g_focus == combo;
  }
  if (!IsOwnerDrawCombo(style)) return;
  int rc[4];
  ClientRect(combo, rc);
  const int field[4] = {rc[0] + 3, rc[1] + 3, rc[2] - 3 - 16, rc[3] - 3};  // inside the edge, before the arrow
  DrawItem(combo, combo, ODT_COMBOBOX, uint32_t(cur), ODA_DRAWENTIRE,
           ODS_COMBOBOXEDIT | (focus ? ODS_FOCUS | ODS_SELECTED : 0u), field, data);
}

void ComboCloseUp(Ctx& c, uint32_t combo, bool ok) {
  uint32_t list;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(combo);
    if (!w || !w->dropped) return;
    w->dropped = false;
    list = w->popup;
    w->popup = 0;
    if (g_capture == list) g_capture = 0;
  }
  Notify(combo, ok ? CBN_SELENDOK : CBN_SELENDCANCEL);
  if (list) CallGuest(c, ThunkFor("user32.dll", "DestroyWindow"), {list});
  Notify(combo, CBN_CLOSEUP);
  Invalidate(combo, nullptr, true);
}

void ComboDropDown(Ctx& c, uint32_t combo) {
  int x, y, w, items, drop_h, ih;
  uint32_t top_level;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* cw = Win(combo);
    if (!cw || cw->dropped) return;
    items = int(cw->items.size());
    drop_h = cw->page;  // the list height the template gave
    ih = cw->item_h > 0 ? cw->item_h : kTextHeight;
    w = cw->w;
  }
  Notify(combo, CBN_DROPDOWN);
  top_level = TopLevel(combo);
  int cx, cy, tx, ty;
  WindowOrigin(combo, &cx, &cy);
  WindowOrigin(top_level, &tx, &ty);
  int ch;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* cw = Win(combo);
    ch = cw ? cw->h : 0;
  }
  x = cx - tx;
  y = cy - ty + ch;
  const int list_h = std::max(ih, std::min(drop_h, std::max(1, items) * ih + 2));
  const uint32_t list = CreateWindowHost(0, "combolbox", "", WS_CHILD | WS_VISIBLE, x, y, w,
                                         list_h, top_level, 0, 0, 0);
  if (!list) return;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* cw = Win(combo);
    Window* lw = Win(list);
    if (!cw || !lw) return;
    lw->userdata = combo;  // its combo box
    lw->item_h = ih;
    lw->top = std::max(0, cw->cur - (list_h / ih) + 1);
    cw->dropped = true;
    cw->popup = list;
    g_capture = list;  // a click outside closes it
  }
  RaiseChild(list);
  (void)c;
}

// The drop-down list's combo box, or 0.
uint32_t ComboOf(uint32_t list) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(list);
  return w ? w->userdata : 0;
}

HLE_NAMED("user32.dll", "<combobox>", ComboBoxWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3), r = 0;
  uint32_t style;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    style = w->style;
    if (msg == WM_GETTEXT || msg == WM_GETTEXTLENGTH) {
      // The selected item's text.
      const std::string t = w->cur >= 0 && w->cur < int(w->items.size())
                                ? w->items[size_t(w->cur)].text
                                : "";
      if (msg == WM_GETTEXTLENGTH) return uint32_t(t.size());
      if (!wp) return 0;
      const std::string s = t.substr(0, wp - 1);
      PutStr(lp, s);
      return uint32_t(s.size());
    }
    if (msg != WM_SETTEXT && CommonControlMsg(*w, msg, wp, lp, &r)) return r;
  }
  const List li{hwnd, !IsOwnerDrawCombo(style) || (style & CBS_HASSTRINGS),
                (style & CBS_SORT) != 0};
  if (ListMsg(c, hwnd, li, ODT_COMBOBOX, false, msg, wp, lp, &r)) return r;
  switch (msg) {
    case WM_CREATE: {
      int rc[4];
      ClientRect(hwnd, rc);
      int field_h = kTextHeight, item_h = kTextHeight;
      if (IsOwnerDrawCombo(style)) {
        field_h = MeasureItem(hwnd, ODT_COMBOBOX, 0xFFFFFFFF, rc[2], kTextHeight);
        item_h = MeasureItem(hwnd, ODT_COMBOBOX, 0, rc[2], kTextHeight);
      }
      std::lock_guard<std::recursive_mutex> l(g_mu);
      if (Window* w = Win(hwnd)) {
        w->item_h = item_h;
        if ((style & CBS_TYPEMASK) != CBS_SIMPLE) {
          w->page = std::max(0, w->h - (field_h + 6));  // the list's share
          w->h = field_h + 6;                          // the field and its edge
        }
      }
      return 0;
    }
    case WM_PAINT:
      Validate(hwnd);
      ComboPaintField(hwnd);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      Focus(c, hwnd);
      bool dropped;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(hwnd);
        dropped = w && w->dropped;
      }
      if (dropped)
        ComboCloseUp(c, hwnd, false);
      else
        ComboDropDown(c, hwnd);
      return 0;
    }
    case WM_KEYDOWN: {
      int cur, n;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(hwnd);
        cur = w ? w->cur : -1;
        n = w ? int(w->items.size()) : 0;
      }
      int to = cur;
      if (wp == 0x26) to = cur - 1;
      else if (wp == 0x28) to = cur + 1;
      else if (wp == 0x0D || wp == 0x1B) { ComboCloseUp(c, hwnd, wp == 0x0D); return 0; }
      else return 0;
      to = std::clamp(to, 0, std::max(0, n - 1));
      if (n && to != cur) {
        {
          std::lock_guard<std::recursive_mutex> l(g_mu);
          if (Window* w = Win(hwnd)) w->cur = to;
        }
        Invalidate(hwnd, nullptr, true);
        Notify(hwnd, CBN_SELCHANGE);
      }
      return 0;
    }
    case WM_SETFOCUS:
      Invalidate(hwnd, nullptr, false);
      Notify(hwnd, CBN_SETFOCUS);
      return 0;
    case WM_KILLFOCUS:
      ComboCloseUp(c, hwnd, false);
      Invalidate(hwnd, nullptr, false);
      Notify(hwnd, CBN_KILLFOCUS);
      return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case 0x014F:  // CB_SHOWDROPDOWN(fShow)
      if (wp) ComboDropDown(c, hwnd);
      else ComboCloseUp(c, hwnd, false);
      return 1;
    case 0x0157: {  // CB_GETDROPPEDSTATE
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      return w && w->dropped ? 1 : 0;
    }
    case 0x0152: {  // CB_GETDROPPEDCONTROLRECT(0, lpRect): screen coordinates
      int x, y, w = 0, h = 0, page = 0;
      WindowOrigin(hwnd, &x, &y);
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        if (Window* cw = Win(hwnd)) w = cw->w, h = cw->h, page = cw->page;
      }
      const int out[4] = {x, y, x + w, y + h + page};
      for (int k = 0; k < 4; k++) Wr32(lp + 4 * k, uint32_t(out[k]));
      return 1;
    }
    case 0x0155: return 0;  // CB_SETEXTENDEDUI
    case 0x0156: return 0;  // CB_GETEXTENDEDUI
    case 0x0141: return 1;  // CB_LIMITTEXT
    case 0x015F: {          // CB_GETDROPPEDWIDTH
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      return w ? uint32_t(w->w) : LB_ERR;
    }
    case 0x0160: return wp;  // CB_SETDROPPEDWIDTH
    case 0x0161: return 0;   // CB_INITSTORAGE
  }
  if (msg >= 0x0140 && msg <= 0x016F) {
    UnhandledControlMsg("combobox", msg);
    return 0;
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

// The drop-down list of a combo box: shows the combo box's items, drawn by
// the combo box's parent; a click on an item selects it and closes the
// list, a click elsewhere just closes it (the list has the capture).
HLE_NAMED("user32.dll", "<combolbox>", ComboLBoxWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3);
  const uint32_t combo = ComboOf(hwnd);
  switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CREATE: return 0;
    case WM_PAINT:
      Validate(hwnd);
      if (combo) {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* lw = Win(hwnd);
        Window* cw = Win(combo);
        if (lw && cw) lw->items = cw->items, lw->cur = cw->cur;
      }
      if (combo) PaintList(combo, hwnd, ODT_COMBOBOX, hwnd);
      return 0;
    case WM_MOUSEMOVE: {  // the item under the pointer is highlighted
      if (!PointInClient(hwnd, lp)) return 0;
      int top;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(hwnd);
        top = w ? w->top : 0;
      }
      const int i = top + HiWord(lp) / ItemHeight(hwnd);
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      if (w && i < int(w->items.size()) && i != w->cur) {
        w->cur = i;
        Invalidate(hwnd, nullptr, true);
      }
      return 0;
    }
    case WM_LBUTTONUP:
    case WM_LBUTTONDOWN: {
      if (!combo) return 0;
      if (!PointInClient(hwnd, lp)) {
        // A click on the combo box itself is the combo box's (it closes).
        if (msg == WM_LBUTTONDOWN) ComboCloseUp(c, combo, false);
        return 0;
      }
      if (msg == WM_LBUTTONDOWN) return 0;
      int top, n, old;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(hwnd);
        Window* cw = Win(combo);
        top = w ? w->top : 0;
        n = cw ? int(cw->items.size()) : 0;
        old = cw ? cw->cur : -1;
      }
      const int i = top + HiWord(lp) / ItemHeight(hwnd);
      if (i >= 0 && i < n && i != old) {
        {
          std::lock_guard<std::recursive_mutex> l(g_mu);
          if (Window* cw = Win(combo)) cw->cur = i;
        }
        Notify(combo, CBN_SELCHANGE);
      }
      ComboCloseUp(c, combo, true);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      const int top = w ? w->top : 0;
      ListScrollTo(hwnd, top - HiWord(wp) / 120);
      return 0;
    }
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

// ---- scroll bar ---------------------------------------------------------------------------

constexpr uint32_t SBM_SETPOS = 0x00E0, SBM_GETPOS = 0x00E1, SBM_SETRANGE = 0x00E2,
                   SBM_SETRANGEREDRAW = 0x00E6, SBM_GETRANGE = 0x00E3;

HLE_NAMED("user32.dll", "<scrollbar>", ScrollBarWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3);
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    switch (msg) {
      case WM_NCCREATE: return 1;
      case WM_CREATE: w->range_lo = 0; w->range_hi = 100; return 0;
      case SBM_SETPOS: {
        const uint32_t old = uint32_t(w->pos);
        w->pos = std::clamp(int(int32_t(wp)), w->range_lo, w->range_hi);
        if (lp) Invalidate(hwnd, nullptr, true);
        return old;
      }
      case SBM_GETPOS: return uint32_t(w->pos);
      case SBM_SETRANGE: case SBM_SETRANGEREDRAW:
        w->range_lo = int(int32_t(wp));
        w->range_hi = int(int32_t(lp));
        w->pos = std::clamp(w->pos, w->range_lo, w->range_hi);
        return uint32_t(w->pos);
      case SBM_GETRANGE:
        if (wp) Wr32(wp, uint32_t(w->range_lo));
        if (lp) Wr32(lp, uint32_t(w->range_hi));
        return 0;
    }
  }
  if (msg == WM_PAINT) {
    Validate(hwnd);
    return 0;
  }
  if (msg >= 0x00E0 && msg <= 0x00EF) {
    UnhandledControlMsg("scrollbar", msg);
    return 0;
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

// ---- trackbar (comctl32) ------------------------------------------------------------------
//
// The thumb's position maps linearly onto the client width (TBS_VERT:
// height). Dragging sends WM_HSCROLL (WM_VSCROLL) with TB_THUMBTRACK, the
// release TB_THUMBPOSITION and TB_ENDTRACK; clicking the channel moves a
// page.

constexpr uint32_t TBS_VERT = 0x2;
constexpr uint32_t TBM_GETPOS = 0x0400, TBM_GETRANGEMIN = 0x0401, TBM_GETRANGEMAX = 0x0402,
                   TBM_SETTIC = 0x0404, TBM_SETPOS = 0x0405, TBM_SETRANGE = 0x0406,
                   TBM_SETRANGEMIN = 0x0407, TBM_SETRANGEMAX = 0x0408,
                   TBM_CLEARTICS = 0x0409, TBM_SETSEL = 0x040A, TBM_SETTICFREQ = 0x0414,
                   TBM_SETPAGESIZE = 0x0415, TBM_GETPAGESIZE = 0x0416,
                   TBM_SETLINESIZE = 0x0417, TBM_GETLINESIZE = 0x0418,
                   TBM_SETTHUMBLENGTH = 0x041B, TBM_GETTHUMBLENGTH = 0x041C;
constexpr uint32_t TB_LINEUP = 0, TB_LINEDOWN = 1, TB_PAGEUP = 2, TB_PAGEDOWN = 3,
                   TB_THUMBPOSITION = 4, TB_THUMBTRACK = 5, TB_TOP = 6, TB_BOTTOM = 7,
                   TB_ENDTRACK = 8;

void TrackNotify(uint32_t hwnd, uint32_t code, int pos, bool vert) {
  if (uint32_t p = Parent(hwnd))
    Send(p, vert ? WM_VSCROLL : WM_HSCROLL, code | (uint32_t(uint16_t(pos)) << 16), hwnd);
}

int TrackPosAt(uint32_t hwnd, uint32_t lp) {
  int rc[4];
  ClientRect(hwnd, rc);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w) return 0;
  const bool vert = w->style & TBS_VERT;
  const int span = std::max(1, (vert ? rc[3] : rc[2]) - 1);
  const int v = std::clamp(vert ? HiWord(lp) : LoWord(lp), 0, span);
  return w->range_lo + int(int64_t(v) * (w->range_hi - w->range_lo) / span);
}

bool TrackSet(uint32_t hwnd, int pos) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w) return false;
  pos = std::clamp(pos, w->range_lo, w->range_hi);
  if (pos == w->pos) return false;
  w->pos = pos;
  Invalidate(hwnd, nullptr, true);
  return true;
}

HLE_NAMED("user32.dll", "<trackbar>", TrackBarWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3);
  bool vert;
  int pos, page;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    vert = w->style & TBS_VERT;
    switch (msg) {
      case WM_NCCREATE: return 1;
      case WM_CREATE:
        w->range_lo = 0, w->range_hi = 100, w->pos = 0, w->line = 1, w->page = 20;
        return 0;
      case TBM_GETPOS: return uint32_t(w->pos);
      case TBM_GETRANGEMIN: return uint32_t(w->range_lo);
      case TBM_GETRANGEMAX: return uint32_t(w->range_hi);
      case TBM_SETRANGE:
        w->range_lo = LoWord(lp), w->range_hi = HiWord(lp);
        w->pos = std::clamp(w->pos, w->range_lo, w->range_hi);
        if (wp) Invalidate(hwnd, nullptr, true);
        return 0;
      case TBM_SETRANGEMIN:
        w->range_lo = int(int32_t(lp));
        w->pos = std::max(w->pos, w->range_lo);
        if (wp) Invalidate(hwnd, nullptr, true);
        return 0;
      case TBM_SETRANGEMAX:
        w->range_hi = int(int32_t(lp));
        w->pos = std::min(w->pos, w->range_hi);
        if (wp) Invalidate(hwnd, nullptr, true);
        return 0;
      case TBM_SETPOS:
        w->pos = std::clamp(int(int32_t(lp)), w->range_lo, w->range_hi);
        if (wp) Invalidate(hwnd, nullptr, true);
        return 0;
      case TBM_SETPAGESIZE: { const uint32_t o = uint32_t(w->page); w->page = int(int32_t(lp)); return o; }
      case TBM_GETPAGESIZE: return uint32_t(w->page);
      case TBM_SETLINESIZE: { const uint32_t o = uint32_t(w->line); w->line = int(int32_t(lp)); return o; }
      case TBM_GETLINESIZE: return uint32_t(w->line);
      case TBM_SETTIC: case TBM_SETTICFREQ: case TBM_CLEARTICS: case TBM_SETSEL:
      case TBM_SETTHUMBLENGTH:
        return 1;
      case TBM_GETTHUMBLENGTH: return uint32_t(vert ? w->w : w->h) / 2;
    }
    pos = w->pos;
    page = w->page;
  }
  switch (msg) {
    case WM_PAINT: Validate(hwnd); return 0;
    case WM_LBUTTONDOWN: {
      Focus(c, hwnd);
      Capture(hwnd);
      if (TrackSet(hwnd, TrackPosAt(hwnd, lp)))
        TrackNotify(hwnd, TB_THUMBTRACK, TrackPosAt(hwnd, lp), vert);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (HasCapture(hwnd) && TrackSet(hwnd, TrackPosAt(hwnd, lp))) {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(hwnd);
        TrackNotify(hwnd, TB_THUMBTRACK, w ? w->pos : 0, vert);
      }
      return 0;
    case WM_LBUTTONUP:
      if (!HasCapture(hwnd)) return 0;
      ReleaseCaptureOf(hwnd);
      TrackNotify(hwnd, TB_THUMBPOSITION, pos, vert);
      TrackNotify(hwnd, TB_ENDTRACK, 0, vert);
      return 0;
    case WM_KEYDOWN: {
      uint32_t code;
      int to = pos;
      switch (wp) {
        case 0x25: case 0x26: code = TB_LINEUP; to = pos - 1; break;    // left, up
        case 0x27: case 0x28: code = TB_LINEDOWN; to = pos + 1; break;  // right, down
        case 0x21: code = TB_PAGEUP; to = pos - page; break;
        case 0x22: code = TB_PAGEDOWN; to = pos + page; break;
        case 0x24: code = TB_TOP; to = INT32_MIN / 2; break;
        case 0x23: code = TB_BOTTOM; to = INT32_MAX / 2; break;
        default: return 0;
      }
      TrackSet(hwnd, to);
      TrackNotify(hwnd, code, 0, vert);
      TrackNotify(hwnd, TB_ENDTRACK, 0, vert);
      return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
  }
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

// ---- progress bar (comctl32) --------------------------------------------------------------

HLE_NAMED("user32.dll", "<progress>", ProgressWndProc, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2), lp = Arg(c, 3);
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    const uint32_t old = uint32_t(w->pos);
    switch (msg) {
      case WM_NCCREATE: return 1;
      case WM_CREATE: w->range_lo = 0, w->range_hi = 100, w->pos = 0, w->line = 10; return 0;
      case 0x0401:  // PBM_SETRANGE(0, MAKELPARAM(min, max))
        w->range_lo = LoWord(lp), w->range_hi = HiWord(lp);
        return MkLparam(0, 100);
      case 0x0406: w->range_lo = int(int32_t(wp)), w->range_hi = int(int32_t(lp)); return 0;
      case 0x0402: w->pos = int(int32_t(wp)); break;               // PBM_SETPOS
      case 0x0403: w->pos += int(int32_t(wp)); break;              // PBM_DELTAPOS
      case 0x0404: { const uint32_t s = uint32_t(w->line); w->line = int(int32_t(wp)); return s; }
      case 0x0405: w->pos += w->line; break;                       // PBM_STEPIT
      case 0x0408: return uint32_t(w->pos);                        // PBM_GETPOS
      case 0x0407:                                                 // PBM_GETRANGE
        if (lp) Wr32(lp, uint32_t(w->range_lo)), Wr32(lp + 4, uint32_t(w->range_hi));
        return uint32_t(wp ? w->range_lo : w->range_hi);
      default:
        if (msg == WM_PAINT) {
          w->dirty = w->erase = false;
          return 0;
        }
        goto def;
    }
    w->pos = std::clamp(w->pos, w->range_lo, w->range_hi);
    Invalidate(hwnd, nullptr, true);
    return old;
  }
def:
  return CallDefWindowProc(c, hwnd, msg, wp, lp);
}

}  // namespace
}  // namespace recomp::hle
