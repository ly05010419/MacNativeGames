// user32 dialog boxes: dialog templates, the dialog class ("#32770") and
// its procedure (DefDlgProc), modeless and modal dialogs, and the dialog
// keyboard interface (IsDialogMessage).
//
// A template (DLGTEMPLATE or DLGTEMPLATEEX) gives the dialog and its
// controls in dialog units: x * base_x / 4 and y * base_y / 8 pixels, the
// base units being the average character size of the dialog font. The
// dialog procedure is kept where Windows keeps it, in the dialog window's
// extra bytes (DWL_DLGPROC at 4, DWL_MSGRESULT at 0, DWL_USER at 8), so
// GetWindowLong / SetWindowLong reach it.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/input_state.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_internal.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

using namespace user32;
using namespace win32;

constexpr uint32_t DS_SETFONT = 0x40, DS_CENTER = 0x800, DS_CONTROL = 0x400;
constexpr uint32_t IDOK = 1, IDCANCEL = 2;
constexpr uint32_t DLGC_WANTARROWS = 0x1, DLGC_WANTTAB = 0x2, DLGC_WANTALLKEYS = 0x4,
                   DLGC_WANTCHARS = 0x80;
constexpr uint8_t VK_TAB = 0x09, VK_RETURN = 0x0D, VK_ESCAPE = 0x1B;

// ---- templates -----------------------------------------------------------------------

struct TemplateItem {
  uint32_t style = 0, exstyle = 0, id = 0;
  int x = 0, y = 0, cx = 0, cy = 0;
  std::string cls;    // lower-case class name ("button", ...)
  std::string title;  // text, or "#n" for a resource ordinal
  gaddr data = 0;     // creation data (passed as lpCreateParams), or 0
};

struct Template {
  uint32_t style = 0, exstyle = 0;
  int x = 0, y = 0, cx = 0, cy = 0;
  std::string cls, title, font;
  int point_size = 0;
  std::vector<TemplateItem> items;
};

// Reads a sz_Or_Ord field at *p (UTF-16 string, 0xFFFF + ordinal, or 0)
// and moves *p past it.
std::string SzOrOrd(gaddr* p) {
  const uint16_t first = Rd16(*p);
  if (first == 0xFFFF) {
    const uint16_t ord = Rd16(*p + 2);
    *p += 4;
    return "#" + std::to_string(ord);
  }
  std::u16string w;
  for (uint16_t ch; (ch = Rd16(*p)) != 0; *p += 2) w.push_back(char16_t(ch));
  *p += 2;
  return Narrow(w);
}

gaddr Align4(gaddr p) { return (p + 3) & ~gaddr(3); }

// Control class atoms of DLGITEMTEMPLATE (0x80 button ... 0x85 combobox).
std::string ItemClass(const std::string& c) {
  static const char* const kAtoms[] = {"button", "edit", "static", "listbox", "scrollbar",
                                       "combobox"};
  if (!c.empty() && c[0] == '#') {
    const unsigned atom = unsigned(std::stoul(c.substr(1)));
    if (atom >= 0x80 && atom <= 0x85) return kAtoms[atom - 0x80];
    return c;
  }
  return Lower(c);
}

Template ParseTemplate(gaddr t) {
  Template d;
  const bool ex = Rd16(t) == 1 && Rd16(t + 2) == 0xFFFF;
  gaddr p;
  uint16_t count;
  if (ex) {  // dlgVer, signature, helpID, exStyle, style, cDlgItems, x, y, cx, cy
    d.exstyle = Rd32(t + 8);
    d.style = Rd32(t + 12);
    count = Rd16(t + 16);
    d.x = int16_t(Rd16(t + 18)), d.y = int16_t(Rd16(t + 20));
    d.cx = int16_t(Rd16(t + 22)), d.cy = int16_t(Rd16(t + 24));
    p = t + 26;
  } else {  // style, dwExtendedStyle, cdit, x, y, cx, cy
    d.style = Rd32(t);
    d.exstyle = Rd32(t + 4);
    count = Rd16(t + 8);
    d.x = int16_t(Rd16(t + 10)), d.y = int16_t(Rd16(t + 12));
    d.cx = int16_t(Rd16(t + 14)), d.cy = int16_t(Rd16(t + 16));
    p = t + 18;
  }
  SzOrOrd(&p);  // menu: dialogs have no menu bar here
  d.cls = SzOrOrd(&p);
  d.title = SzOrOrd(&p);
  if (d.style & DS_SETFONT) {
    d.point_size = Rd16(p);
    p += ex ? 8 : 2;  // pointsize (+ weight, italic, charset)
    d.font = SzOrOrd(&p);
  }
  for (uint16_t i = 0; i < count; i++) {
    p = Align4(p);
    TemplateItem it;
    if (ex) {  // helpID, exStyle, style, x, y, cx, cy, id (DWORD)
      it.exstyle = Rd32(p + 4);
      it.style = Rd32(p + 8);
      it.x = int16_t(Rd16(p + 12)), it.y = int16_t(Rd16(p + 14));
      it.cx = int16_t(Rd16(p + 16)), it.cy = int16_t(Rd16(p + 18));
      it.id = Rd32(p + 20);
      p += 24;
    } else {  // style, dwExtendedStyle, x, y, cx, cy, id (WORD)
      it.style = Rd32(p);
      it.exstyle = Rd32(p + 4);
      it.x = int16_t(Rd16(p + 8)), it.y = int16_t(Rd16(p + 10));
      it.cx = int16_t(Rd16(p + 12)), it.cy = int16_t(Rd16(p + 14));
      it.id = Rd16(p + 16);
      p += 18;
    }
    it.cls = ItemClass(SzOrOrd(&p));
    it.title = SzOrOrd(&p);
    const uint16_t extra = Rd16(p);
    // The creation data starts with its size (a WORD) and is handed over
    // as is (lpCreateParams points at the size).
    if (extra) it.data = p;
    p += 2 + extra;
    d.items.push_back(it);
  }
  return d;
}

// Dialog base units of a template font: the average character width and
// the height of the font, in pixels at 96 DPI. Measured for MS Sans Serif
// and MS Shell Dlg at 8 points (6 x 13); other sizes scale from it. With
// no font the system font's (8 x 16).
void BaseUnits(const Template& d, int* bx, int* by) {
  if (!(d.style & DS_SETFONT) || d.point_size <= 0) {
    *bx = 8;
    *by = 16;
    return;
  }
  *bx = std::max(1, int(std::lround(6.0 * d.point_size / 8.0)));
  *by = std::max(1, int(std::lround(13.0 * d.point_size / 8.0)));
}

// ---- the dialog window -------------------------------------------------------------

uint32_t GetDefId(uint32_t dlg) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(dlg);
  return w ? w->def_id : IDOK;
}

void SetDefId(uint32_t dlg, uint32_t id) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(dlg)) w->def_id = id;
}

// PostQuitMessage for the calling thread (a modal loop hands WM_QUIT on).
void PostQuitMessageFor(uint32_t code) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Queue& q = g_queues[CurrentThread()->tid];
    q.quit = true;
    q.quit_code = code;
  }
  g_queue_cv.notify_all();
}

uint32_t DlgProc(uint32_t dlg) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(dlg);
  if (!w || w->extra.size() < 12) return 0;
  uint32_t v;
  std::memcpy(&v, w->extra.data() + DWL_DLGPROC, 4);
  return v;
}

void SetDlgLong(uint32_t dlg, int32_t index, uint32_t v) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(dlg);
  if (w && w->extra.size() >= uint32_t(index) + 4) std::memcpy(w->extra.data() + index, &v, 4);
}

uint32_t GetDlgLong(uint32_t dlg, int32_t index) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(dlg);
  uint32_t v = 0;
  if (w && w->extra.size() >= uint32_t(index) + 4) std::memcpy(&v, w->extra.data() + index, 4);
  return v;
}

bool IsTabStop(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  return w && w->visible && w->enabled && (w->style & WS_TABSTOP);
}

// The next (or previous) WS_TABSTOP control after `from` in the dialog's
// control order, wrapping around; `from` 0 starts at the first.
uint32_t NextTabItem(uint32_t dlg, uint32_t from, bool previous) {
  std::vector<uint32_t> kids = Children(dlg);
  if (kids.empty()) return 0;
  if (previous) std::reverse(kids.begin(), kids.end());
  auto it = std::find(kids.begin(), kids.end(), from);
  const size_t start = it == kids.end() ? kids.size() - 1 : size_t(it - kids.begin());
  for (size_t i = 1; i <= kids.size(); i++) {
    const uint32_t k = kids[(start + i) % kids.size()];
    if (IsTabStop(k)) return k;
  }
  return 0;
}

uint32_t DlgItem(uint32_t dlg, uint32_t id) {
  for (uint32_t k : Children(dlg)) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(k); w && w->id == (id & 0xFFFF)) return k;
  }
  return 0;
}

// Creates a dialog from template `t` (guest memory). The dialog is shown
// at the end when its style has WS_VISIBLE (a modal dialog always is).
uint32_t CreateDialogFrom(Ctx& c, uint32_t hinst, gaddr t, uint32_t parent, uint32_t proc,
                          uint32_t param, bool modal) {
  const Template d = ParseTemplate(t);
  int bx, by;
  BaseUnits(d, &bx, &by);
  auto px = [bx](int v) { return v * bx / 4; };
  auto py = [by](int v) { return v * by / 8; };
  int x = px(d.x), y = py(d.y), cw = px(d.cx), ch = py(d.cy);
  // The window rect around a client area of cw x ch (top-level frame).
  int w = cw, h = ch;
  const uint32_t style = d.style & ~WS_VISIBLE;
  if (!(style & WS_CHILD)) {
    int border, caption;
    FrameSize(style, &border, &caption);
    w += 2 * border;
    h += 2 * border + caption;
  }
  if (d.style & DS_CENTER) {
    int pw = 0, ph = 0;
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* pwin = parent ? Win(parent) : nullptr;
      if (pwin && (style & WS_CHILD)) {
        ClientSize(*pwin, &pw, &ph);
      } else if (pwin) {
        pw = pwin->w, ph = pwin->h;
        x = pwin->x, y = pwin->y;
      } else {
        int dw = 0, dh = 0;
        DesktopSize(&dw, &dh);
        pw = dw, ph = dh;
        x = y = 0;
      }
    }
    if (style & WS_CHILD) x = y = 0;
    x += (pw - w) / 2;
    y += (ph - h) / 2;
  } else if (!(style & WS_CHILD) && parent) {
    // A top-level dialog is placed relative to its owner's client area.
    int ox, oy;
    WindowOrigin(parent, &ox, &oy);
    x += ox;
    y += oy;
  }
  EnsureSystemClasses();
  const std::string cls = d.cls.empty() ? "#32770" : d.cls;
  const uint32_t dlg =
      CreateWindowHost(d.exstyle, cls, d.title, style, x, y, w, h, parent, 0, hinst, param);
  if (!dlg) return 0;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* dw = Win(dlg)) {
      dw->dialog = true;
      if (dw->extra.size() < 30) dw->extra.resize(30, 0);
    }
  }
  SetDlgLong(dlg, DWL_DLGPROC, proc);
  // DS_SETFONT: the dialog and each control get the template's font
  // (WM_SETFONT) before WM_INITDIALOG.
  uint32_t font = 0;
  if ((d.style & DS_SETFONT) && !d.font.empty()) {
    const gaddr face = HeapAllocProcess(d.font.size() + 1, false);
    PutStr(face, d.font);
    const int height = -(d.point_size * 96 + 36) / 72;  // pixels at 96 DPI, as character height
    font = uint32_t(CallGuest(c, ThunkFor("gdi32.dll", "CreateFontA"),
                              {uint32_t(height), 0, 0, 0, 400, 0, 0, 0, 1 /* DEFAULT_CHARSET */,
                               0, 0, 0, 0, face}));
    HeapFreeProcess(face);
    Send(dlg, WM_SETFONT, font, 0);
  }
  for (const TemplateItem& it : d.items) {
    const uint32_t k = CreateWindowHost(it.exstyle, it.cls, it.title, it.style | WS_CHILD,
                                        px(it.x), py(it.y), px(it.cx), py(it.cy), dlg, it.id,
                                        hinst, uint32_t(it.data));
    if (k && font) Send(k, WM_SETFONT, font, 0);
    if (!k) {
      WarnF("dialog: control %u (%s) not created\n", it.id, it.cls.c_str());
      if (!(d.style & 0x4)) {  // DS_NOFAILCREATE
        Send(dlg, WM_DESTROY, 0, 0);
        return 0;
      }
    }
  }
  // WM_INITDIALOG: TRUE asks for the focus on the first tab stop.
  const uint32_t first = NextTabItem(dlg, 0, false);
  uint32_t focus = 0;
  if (proc && CallGuest(c, proc, {dlg, WM_INITDIALOG, first, param})) focus = first;
  bool show;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* dw = Win(dlg);
    if (!dw) return 0;  // destroyed by its own WM_INITDIALOG
    show = modal || (d.style & WS_VISIBLE);
  }
  if (show) {
    const uint32_t sw = ProcAddress(ModuleHandle("user32.dll"), "ShowWindow");
    CallGuest(c, sw, {dlg, (style & WS_CHILD) ? 5u /* SW_SHOW */ : 1u /* SW_SHOWNORMAL */});
  }
  if (focus) {
    const uint32_t sf = ProcAddress(ModuleHandle("user32.dll"), "SetFocus");
    CallGuest(c, sf, {focus});
  }
  return dlg;
}

// The dialog template resource `name` (a string or MAKEINTRESOURCE) of
// module `hinst`, or 0.
gaddr FindDialogTemplate(Ctx& c, uint32_t hinst, uint32_t name) {
  const gaddr find = ThunkFor("kernel32.dll", "FindResourceA");
  const gaddr load = ThunkFor("kernel32.dll", "LoadResource");
  const uint32_t res = uint32_t(CallGuest(c, find, {hinst, name, 5 /* RT_DIALOG */}));
  if (!res) return 0;
  return gaddr(CallGuest(c, load, {hinst, res}));
}

// A modal dialog's message loop: until EndDialog. The owner is disabled
// meanwhile, as DialogBox does.
uint32_t RunModal(Ctx& c, uint32_t dlg, uint32_t owner) {
  const gaddr get = ThunkFor("user32.dll", "GetMessageA");
  const gaddr isdlg = ThunkFor("user32.dll", "IsDialogMessageA");
  const gaddr translate = ThunkFor("user32.dll", "TranslateMessage");
  const gaddr dispatch = ThunkFor("user32.dll", "DispatchMessageA");
  const gaddr enable = ThunkFor("user32.dll", "EnableWindow");
  const gaddr destroy = ThunkFor("user32.dll", "DestroyWindow");
  if (owner) CallGuest(c, enable, {owner, 0});
  const gaddr msg = HeapAllocProcess(28, true);
  for (;;) {
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(dlg);
      if (!w || w->ended) break;
    }
    if (!CallGuest(c, get, {msg, 0, 0, 0})) {
      // WM_QUIT: put it back for the outer loop and leave.
      PostQuitMessageFor(Rd32(msg + 8));
      break;
    }
    if (!CallGuest(c, isdlg, {dlg, msg})) {
      CallGuest(c, translate, {msg});
      CallGuest(c, dispatch, {msg});
    }
  }
  HeapFreeProcess(msg);
  uint32_t result = 0xFFFFFFFF;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(dlg)) result = w->end_result;
  }
  if (owner) CallGuest(c, enable, {owner, 1});
  CallGuest(c, destroy, {dlg});
  return result;
}

// ---- exports --------------------------------------------------------------------------

// (hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam)
HLE("user32.dll", CreateDialogIndirectParamA, 5) {
  return CreateDialogFrom(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), false);
}

// (hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam)
HLE("user32.dll", CreateDialogParamA, 5) {
  const gaddr t = FindDialogTemplate(c, Arg(c, 0), Arg(c, 1));
  if (!t) {
    SetLastError(kErrorResourceNameNotFound);
    return 0;
  }
  return CreateDialogFrom(c, Arg(c, 0), t, Arg(c, 2), Arg(c, 3), Arg(c, 4), false);
}

// (hInstance, hDialogTemplate, hWndParent, lpDialogFunc, dwInitParam)
HLE("user32.dll", DialogBoxIndirectParamA, 5) {
  const uint32_t dlg = CreateDialogFrom(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3),
                                        Arg(c, 4), true);
  return dlg ? RunModal(c, dlg, Arg(c, 2)) : 0xFFFFFFFF;
}

// (hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam)
HLE("user32.dll", DialogBoxParamA, 5) {
  const gaddr t = FindDialogTemplate(c, Arg(c, 0), Arg(c, 1));
  if (!t) {
    SetLastError(kErrorResourceNameNotFound);
    return 0xFFFFFFFF;
  }
  const uint32_t dlg = CreateDialogFrom(c, Arg(c, 0), t, Arg(c, 2), Arg(c, 3), Arg(c, 4), true);
  return dlg ? RunModal(c, dlg, Arg(c, 2)) : 0xFFFFFFFF;
}

// (hDlg, nResult): a modal dialog's loop ends with nResult; the dialog
// is hidden at once (DialogBox destroys it).
HLE("user32.dll", EndDialog, 2) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    if (!w) return 0;
    w->ended = true;
    w->end_result = Arg(c, 1);
  }
  const gaddr sw = ThunkFor("user32.dll", "ShowWindow");
  CallGuest(c, sw, {Arg(c, 0), 0});
  return 1;
}

HLE("user32.dll", GetDlgItem, 2) { return DlgItem(Arg(c, 0), Arg(c, 1)); }

// Calls cb(hwnd, lParam) for every descendant of `parent` (children before
// their own children, in Z order); false once the callback stops it.
bool EnumDescendants(Ctx& c, uint32_t parent, uint32_t cb, uint32_t lp) {
  for (uint32_t k : Children(parent)) {
    if (!CallGuest(c, cb, {k, lp})) return false;
    if (!EnumDescendants(c, k, cb, lp)) return false;
  }
  return true;
}

// (hWndParent, lpEnumFunc, lParam)
HLE("user32.dll", EnumChildWindows, 3) {
  EnumDescendants(c, Arg(c, 0), Arg(c, 1), Arg(c, 2));
  return 1;
}

HLE("user32.dll", GetDlgCtrlID, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w && (w->style & WS_CHILD) ? w->id : 0;
}

// (hDlg, nIDDlgItem, Msg, wParam, lParam)
HLE("user32.dll", SendDlgItemMessageA, 5) {
  const uint32_t k = DlgItem(Arg(c, 0), Arg(c, 1));
  return k ? Send(k, Arg(c, 2), Arg(c, 3), Arg(c, 4)) : 0;
}

HLE("user32.dll", SetDlgItemTextA, 3) {
  const uint32_t k = DlgItem(Arg(c, 0), Arg(c, 1));
  return k ? (Send(k, WM_SETTEXT, 0, Arg(c, 2)) ? 1 : 0) : 0;
}

HLE("user32.dll", GetDlgItemTextA, 4) {
  const uint32_t k = DlgItem(Arg(c, 0), Arg(c, 1));
  if (!k) {
    if (Arg(c, 3)) Wr8(Arg(c, 2), 0);
    return 0;
  }
  return Send(k, WM_GETTEXT, Arg(c, 3), Arg(c, 2));
}

// (hDlg, nIDButton, uCheck): BM_SETCHECK.
HLE("user32.dll", CheckDlgButton, 3) {
  const uint32_t k = DlgItem(Arg(c, 0), Arg(c, 1));
  if (!k) return 0;
  Send(k, 0x00F1 /* BM_SETCHECK */, Arg(c, 2), 0);
  return 1;
}

// (hDlg, nIDButton): BM_GETCHECK.
HLE("user32.dll", IsDlgButtonChecked, 2) {
  const uint32_t k = DlgItem(Arg(c, 0), Arg(c, 1));
  return k ? Send(k, 0x00F0 /* BM_GETCHECK */, 0, 0) : 0;
}

// (hDlg, hCtl, bPrevious)
HLE("user32.dll", GetNextDlgTabItem, 3) {
  return NextTabItem(Arg(c, 0), Arg(c, 1), Arg(c, 2) != 0);
}

// (hDlg, lpRect): dialog units to pixels with the dialog's base units.
// The template is gone by now; dialogs created from templates here all
// use the base units of their font, recorded nowhere else, so the common
// 8-point dialog font is assumed.
HLE("user32.dll", MapDialogRect, 2) {
  const gaddr r = Arg(c, 1);
  Wr32(r + 0, uint32_t(int32_t(Rd32(r + 0)) * 6 / 4));
  Wr32(r + 4, uint32_t(int32_t(Rd32(r + 4)) * 13 / 8));
  Wr32(r + 8, uint32_t(int32_t(Rd32(r + 8)) * 6 / 4));
  Wr32(r + 12, uint32_t(int32_t(Rd32(r + 12)) * 13 / 8));
  return 1;
}

// The dialog class procedure. The dialog procedure gets every message
// first; a nonzero return means handled, the result then being
// DWL_MSGRESULT, except for the messages whose return value is the
// dialog procedure's own (WM_CTLCOLOR*, WM_INITDIALOG, ...).
uint32_t DefDlg(Ctx& c, uint32_t dlg, uint32_t msg, uint32_t wp, uint32_t lp) {
  if (const uint32_t proc = DlgProc(dlg)) {
    SetDlgLong(dlg, DWL_MSGRESULT, 0);
    const uint32_t r = uint32_t(CallGuest(c, proc, {dlg, msg, wp, lp}));
    if (r) {
      const bool direct = (msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) ||
                          msg == WM_INITDIALOG || msg == 0x0039 /* WM_COMPAREITEM */ ||
                          msg == 0x002E /* WM_VKEYTOITEM */ ||
                          msg == 0x002F /* WM_CHARTOITEM */ || msg == 0x0037;
      return direct ? r : GetDlgLong(dlg, DWL_MSGRESULT);
    }
  }
  switch (msg) {
    case WM_CLOSE:  // as the Cancel button
      Post(dlg, WM_COMMAND, IDCANCEL, DlgItem(dlg, IDCANCEL));
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SETFONT: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      if (Window* w = Win(dlg)) w->font = wp;
      return 0;
    }
    case WM_GETFONT: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(dlg);
      return w ? w->font : 0;
    }
    case WM_SETFOCUS: {  // to the first tab stop
      if (uint32_t k = NextTabItem(dlg, 0, false)) {
        const gaddr sf = ThunkFor("user32.dll", "SetFocus");
        CallGuest(c, sf, {k});
      }
      return 0;
    }
    case WM_NEXTDLGCTL: {
      // wParam: the control (lParam TRUE) or next / previous (lParam FALSE).
      uint32_t k = lp & 0xFFFF ? wp : NextTabItem(dlg, g_focus, wp != 0);
      if (k) {
        const gaddr sf = ThunkFor("user32.dll", "SetFocus");
        CallGuest(c, sf, {k});
      }
      return 0;
    }
    case 0x0400:  // DM_GETDEFID
      return 0x534B0000u | GetDefId(dlg);
    case 0x0401:  // DM_SETDEFID
      SetDefId(dlg, wp);
      return 1;
    default:
      return CallDefWindowProc(c, dlg, msg, wp, lp);
  }
}

HLE_NAMED("user32.dll", "<dialog>", DialogWndProc, 4) {
  return DefDlg(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3));
}

HLE("user32.dll", DefDlgProcA, 4) {
  return DefDlg(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3));
}

// (hDlg, lpMsg): the dialog keyboard interface. Messages for the dialog
// or its controls are processed here (TRUE); Tab moves the focus, Enter
// presses the default button and Escape Cancel, unless the focused
// control asks for those keys (WM_GETDLGCODE).
HLE("user32.dll", IsDialogMessageA, 2) {
  const uint32_t dlg = Arg(c, 0), m = Arg(c, 1);
  const uint32_t hwnd = Rd32(m), msg = Rd32(m + 4), wp = Rd32(m + 8);
  if (!hwnd || !IsSelfOrAncestor(dlg, hwnd)) return 0;
  if (msg == WM_KEYDOWN) {
    const uint32_t code = Send(hwnd, WM_GETDLGCODE, wp, m);
    if (!(code & DLGC_WANTALLKEYS)) {
      if (wp == VK_TAB && !(code & DLGC_WANTTAB)) {
        const bool back = g_key_down[0x10];
        if (uint32_t k = NextTabItem(dlg, hwnd, back)) {
          const gaddr sf = ThunkFor("user32.dll", "SetFocus");
          CallGuest(c, sf, {k});
        }
        return 1;
      }
      if (wp == VK_ESCAPE) {
        Send(dlg, WM_COMMAND, IDCANCEL, DlgItem(dlg, IDCANCEL));
        return 1;
      }
      if (wp == VK_RETURN) {
        const uint32_t id = GetDefId(dlg);
        Send(dlg, WM_COMMAND, id, DlgItem(dlg, id));
        return 1;
      }
    }
  }
  const gaddr translate = ThunkFor("user32.dll", "TranslateMessage");
  const gaddr dispatch = ThunkFor("user32.dll", "DispatchMessageA");
  CallGuest(c, translate, {m});
  CallGuest(c, dispatch, {m});
  return 1;
}

}  // namespace
}  // namespace recomp::hle
