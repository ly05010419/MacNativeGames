// user32: keyboard layout (US), key state, system metrics/parameters,
// message boxes and wsprintfA. Window management and the message loop are in
// user32_window.cpp.

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/input_state.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_window.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace recomp {

std::atomic<uint8_t> g_key_down[256];
std::atomic<uint8_t> g_key_toggled[256];
std::atomic<uint8_t> g_key_pressed_since[256];

namespace {

// US layout. Scan codes are set-1 make codes; `ext` marks E0-prefixed keys.
struct KeyDef {
  uint8_t vk, sc;
  bool ext;
  char ch, shifted;  // characters produced (0 if none)
  const char* name;  // GetKeyNameText
};

const KeyDef kKeys[] = {
    {0x1B, 0x01, false, 0, 0, "Esc"},
    {'1', 0x02, false, '1', '!', "1"}, {'2', 0x03, false, '2', '@', "2"},
    {'3', 0x04, false, '3', '#', "3"}, {'4', 0x05, false, '4', '$', "4"},
    {'5', 0x06, false, '5', '%', "5"}, {'6', 0x07, false, '6', '^', "6"},
    {'7', 0x08, false, '7', '&', "7"}, {'8', 0x09, false, '8', '*', "8"},
    {'9', 0x0A, false, '9', '(', "9"}, {'0', 0x0B, false, '0', ')', "0"},
    {0xBD, 0x0C, false, '-', '_', "-"}, {0xBB, 0x0D, false, '=', '+', "="},
    {0x08, 0x0E, false, 8, 8, "Backspace"}, {0x09, 0x0F, false, 9, 9, "Tab"},
    {'Q', 0x10, false, 'q', 'Q', "Q"}, {'W', 0x11, false, 'w', 'W', "W"},
    {'E', 0x12, false, 'e', 'E', "E"}, {'R', 0x13, false, 'r', 'R', "R"},
    {'T', 0x14, false, 't', 'T', "T"}, {'Y', 0x15, false, 'y', 'Y', "Y"},
    {'U', 0x16, false, 'u', 'U', "U"}, {'I', 0x17, false, 'i', 'I', "I"},
    {'O', 0x18, false, 'o', 'O', "O"}, {'P', 0x19, false, 'p', 'P', "P"},
    {0xDB, 0x1A, false, '[', '{', "["}, {0xDD, 0x1B, false, ']', '}', "]"},
    {0x0D, 0x1C, false, 13, 13, "Enter"}, {0x11, 0x1D, false, 0, 0, "Ctrl"},
    {'A', 0x1E, false, 'a', 'A', "A"}, {'S', 0x1F, false, 's', 'S', "S"},
    {'D', 0x20, false, 'd', 'D', "D"}, {'F', 0x21, false, 'f', 'F', "F"},
    {'G', 0x22, false, 'g', 'G', "G"}, {'H', 0x23, false, 'h', 'H', "H"},
    {'J', 0x24, false, 'j', 'J', "J"}, {'K', 0x25, false, 'k', 'K', "K"},
    {'L', 0x26, false, 'l', 'L', "L"}, {0xBA, 0x27, false, ';', ':', ";"},
    {0xDE, 0x28, false, '\'', '"', "'"}, {0xC0, 0x29, false, '`', '~', "`"},
    {0x10, 0x2A, false, 0, 0, "Shift"}, {0xDC, 0x2B, false, '\\', '|', "\\"},
    {'Z', 0x2C, false, 'z', 'Z', "Z"}, {'X', 0x2D, false, 'x', 'X', "X"},
    {'C', 0x2E, false, 'c', 'C', "C"}, {'V', 0x2F, false, 'v', 'V', "V"},
    {'B', 0x30, false, 'b', 'B', "B"}, {'N', 0x31, false, 'n', 'N', "N"},
    {'M', 0x32, false, 'm', 'M', "M"}, {0xBC, 0x33, false, ',', '<', ","},
    {0xBE, 0x34, false, '.', '>', "."}, {0xBF, 0x35, false, '/', '?', "/"},
    {0x10, 0x36, false, 0, 0, "Right Shift"},
    {0x6A, 0x37, false, '*', '*', "Num *"}, {0x12, 0x38, false, 0, 0, "Alt"},
    {0x20, 0x39, false, ' ', ' ', "Space"},
    {0x14, 0x3A, false, 0, 0, "Caps Lock"},
    {0x70, 0x3B, false, 0, 0, "F1"}, {0x71, 0x3C, false, 0, 0, "F2"},
    {0x72, 0x3D, false, 0, 0, "F3"}, {0x73, 0x3E, false, 0, 0, "F4"},
    {0x74, 0x3F, false, 0, 0, "F5"}, {0x75, 0x40, false, 0, 0, "F6"},
    {0x76, 0x41, false, 0, 0, "F7"}, {0x77, 0x42, false, 0, 0, "F8"},
    {0x78, 0x43, false, 0, 0, "F9"}, {0x79, 0x44, false, 0, 0, "F10"},
    {0x90, 0x45, false, 0, 0, "Num Lock"},
    {0x91, 0x46, false, 0, 0, "Scroll Lock"},
    // Numpad without NumLock reports the navigation VKs (Windows does too
    // for MapVirtualKey(VSC_TO_VK)).
    {0x24, 0x47, false, 0, 0, "Num 7"}, {0x26, 0x48, false, 0, 0, "Num 8"},
    {0x21, 0x49, false, 0, 0, "Num 9"}, {0x6D, 0x4A, false, '-', '-', "Num -"},
    {0x25, 0x4B, false, 0, 0, "Num 4"}, {0x0C, 0x4C, false, 0, 0, "Num 5"},
    {0x27, 0x4D, false, 0, 0, "Num 6"}, {0x6B, 0x4E, false, '+', '+', "Num +"},
    {0x23, 0x4F, false, 0, 0, "Num 1"}, {0x28, 0x50, false, 0, 0, "Num 2"},
    {0x22, 0x51, false, 0, 0, "Num 3"}, {0x2D, 0x52, false, 0, 0, "Num 0"},
    {0x2E, 0x53, false, 0, 0, "Num Del"},
    {0x7A, 0x57, false, 0, 0, "F11"}, {0x7B, 0x58, false, 0, 0, "F12"},
    // Extended keys.
    {0x0D, 0x1C, true, 13, 13, "Num Enter"},
    {0x11, 0x1D, true, 0, 0, "Right Ctrl"},
    {0x6F, 0x35, true, '/', '/', "Num /"},
    {0x2C, 0x37, true, 0, 0, "Prnt Scrn"},
    {0x12, 0x38, true, 0, 0, "Right Alt"},
    {0x13, 0x45, true, 0, 0, "Pause"},
    {0x24, 0x47, true, 0, 0, "Home"}, {0x26, 0x48, true, 0, 0, "Up"},
    {0x21, 0x49, true, 0, 0, "Page Up"}, {0x25, 0x4B, true, 0, 0, "Left"},
    {0x27, 0x4D, true, 0, 0, "Right"}, {0x23, 0x4F, true, 0, 0, "End"},
    {0x28, 0x50, true, 0, 0, "Down"}, {0x22, 0x51, true, 0, 0, "Page Down"},
    {0x2D, 0x52, true, 0, 0, "Insert"}, {0x2E, 0x53, true, 0, 0, "Delete"},
    {0x5B, 0x5B, true, 0, 0, "Left Windows"},
    {0x5C, 0x5C, true, 0, 0, "Right Windows"},
    {0x5D, 0x5D, true, 0, 0, "Application"},
};

// Numpad digits with NumLock on.
const struct { uint8_t vk, sc; } kNumpadVk[] = {
    {0x67, 0x47}, {0x68, 0x48}, {0x69, 0x49}, {0x64, 0x4B}, {0x65, 0x4C},
    {0x66, 0x4D}, {0x61, 0x4F}, {0x62, 0x50}, {0x63, 0x51}, {0x60, 0x52},
    {0x6E, 0x53}};

const KeyDef* BySc(uint8_t sc, bool ext) {
  for (const auto& k : kKeys)
    if (k.sc == sc && k.ext == ext) return &k;
  return nullptr;
}

const KeyDef* ByVk(uint8_t vk) {
  for (const auto& k : kKeys)
    if (k.vk == vk) return &k;
  return nullptr;
}

}  // namespace

namespace {
std::atomic<RawInputSink> g_raw_sink{nullptr};
}  // namespace

void SetRawInputSink(RawInputSink sink) { g_raw_sink = sink; }

void EmitRawInput(RawInput::Kind kind, uint8_t code, int32_t value) {
  if (RawInputSink sink = g_raw_sink) sink({kind, code, value, TickCount()});
}

uint8_t ScanCodeForVk(uint8_t vk, bool* ext) {
  *ext = false;
  switch (vk) {  // left/right specific VKs
    case 0xA0: return 0x2A;
    case 0xA1: return 0x36;
    case 0xA2: return 0x1D;
    case 0xA3: *ext = true; return 0x1D;
    case 0xA4: return 0x38;
    case 0xA5: *ext = true; return 0x38;
  }
  for (const auto& n : kNumpadVk)
    if (n.vk == vk) return n.sc;
  // Prefer the extended navigation key when both exist (arrows etc.).
  for (const auto& k : kKeys)
    if (k.vk == vk && k.ext && (vk >= 0x21 && vk <= 0x2E)) {
      *ext = true;
      return k.sc;
    }
  const KeyDef* k = ByVk(vk);
  if (!k) return 0;
  *ext = k->ext;
  return k->sc;
}

namespace hle {
namespace {

using namespace win32;

HLE("user32.dll", MapVirtualKeyA, 2) {
  uint32_t code = Arg(c, 0), type = Arg(c, 1);
  switch (type) {
    case 0: {  // MAPVK_VK_TO_VSC
      bool ext;
      return ScanCodeForVk(uint8_t(code), &ext);
    }
    case 1:    // MAPVK_VSC_TO_VK
    case 3: {  // MAPVK_VSC_TO_VK_EX
      const KeyDef* k = BySc(uint8_t(code), false);
      if (!k) return 0;
      if (type == 3) {
        if (code == 0x2A) return 0xA0;
        if (code == 0x36) return 0xA1;
        if (code == 0x1D) return 0xA2;
        if (code == 0x38) return 0xA4;
      }
      return k->vk;
    }
    case 2: {  // MAPVK_VK_TO_CHAR: unshifted, letters upper case
      if (code >= 'A' && code <= 'Z') return code;
      for (const auto& n : kNumpadVk)
        if (n.vk == code) return code == 0x6E ? '.' : '0' + (code - 0x60);
      const KeyDef* k = ByVk(uint8_t(code));
      return k && k->ch ? uint32_t(uint8_t(k->ch)) : 0;
    }
  }
  return 0;
}

HLE("user32.dll", VkKeyScanA, 1) {
  char ch = char(Arg(c, 0));
  for (const auto& k : kKeys) {
    if (k.ext) continue;
    if (k.ch == ch) return k.vk;
    if (k.shifted == ch) return 0x100u | k.vk;  // shift
  }
  return 0xFFFF;
}

HLE("user32.dll", GetKeyNameTextA, 3) {
  uint32_t lparam = Arg(c, 0), buf = Arg(c, 1), size = Arg(c, 2);
  const KeyDef* k = BySc(uint8_t(lparam >> 16), (lparam >> 24) & 1);
  if (!k || !size) return 0;
  std::string n = std::string(k->name).substr(0, size - 1);
  PutStr(buf, n);
  return uint32_t(n.size());
}

HLE("user32.dll", GetKeyboardLayout, 1) { return 0x04090409; }

// Key state: bit 15 = down; GetKeyState bit 0 = toggled. GetAsyncKeyState
// bit 0 = "pressed since last call" (still honoured by Windows XP).
HLE("user32.dll", GetKeyState, 1) {
  uint8_t vk = uint8_t(Arg(c, 0));
  uint32_t r = (g_key_down[vk] ? 0xFF80u : 0) | (g_key_toggled[vk] ? 1u : 0);
  return r;
}

HLE("user32.dll", GetAsyncKeyState, 1) {
  uint8_t vk = uint8_t(Arg(c, 0));
  uint32_t r = (g_key_down[vk] ? 0x8000u : 0) |
               (g_key_pressed_since[vk].exchange(0) ? 1u : 0);
  return r;
}

// ---- system metrics and parameters -------------------------------------------

HLE("user32.dll", GetSystemMetrics, 1) {
  switch (Arg(c, 0)) {
    case 0: return uint32_t(g_display_w);   // SM_CXSCREEN
    case 1: return uint32_t(g_display_h);   // SM_CYSCREEN
    case 2: return 16;    // SM_CXVSCROLL
    case 3: return 16;    // SM_CYHSCROLL
    case 4: return 19;    // SM_CYCAPTION
    case 5: return 1;     // SM_CXBORDER
    case 6: return 1;     // SM_CYBORDER
    case 7: return 3;     // SM_CXDLGFRAME
    case 8: return 3;     // SM_CYDLGFRAME
    case 11: return 32;   // SM_CXICON
    case 12: return 32;
    case 13: return 32;   // SM_CXCURSOR
    case 14: return 32;
    case 15: return 19;   // SM_CYMENU
    case 16: return uint32_t(g_display_w);       // SM_CXFULLSCREEN: the work
    case 17: return uint32_t(g_display_h - 19);  // area minus a caption
    case 19: return 1;    // SM_MOUSEPRESENT
    case 23: return 0;    // SM_SWAPBUTTON
    case 32: return 4;    // SM_CXFRAME
    case 33: return 4;
    case 36: return 4;    // SM_CXDOUBLECLK
    case 37: return 4;
    case 43: return 3;    // SM_CMOUSEBUTTONS
    case 75: return 1;    // SM_MOUSEWHEELPRESENT
    case 76: return 0;    // SM_XVIRTUALSCREEN
    case 77: return 0;    // SM_YVIRTUALSCREEN
    case 78: return uint32_t(g_display_w);  // SM_CXVIRTUALSCREEN
    case 79: return uint32_t(g_display_h);
    case 80: return 1;    // SM_CMONITORS
    default:
      WarnF("GetSystemMetrics(%u): returning 0\n", uint32_t(Arg(c, 0)));
      return 0;
  }
}

HLE("user32.dll", GetDoubleClickTime, 0) { return 500; }
HLE("user32.dll", GetCaretBlinkTime, 0) { return 530; }

HLE("user32.dll", GetSysColor, 1) {
  static const uint32_t kColors[31] = {
      0xC8D0D4, 0x6E3A0A, 0x6A240A, 0x808080, 0xC8D0D4, 0xFFFFFF, 0x000000,
      0x000000, 0x000000, 0xFFFFFF, 0xC8D0D4, 0xC8D0D4, 0x808080, 0x6A240A,
      0xFFFFFF, 0xC8D0D4, 0x808080, 0x808080, 0x000000, 0xC8D0D4, 0xFFFFFF,
      0x404040, 0xC8D0D4, 0x000000, 0xE1FFFF, 0xB5B5B5, 0x800000, 0xF0CAA6,
      0xC0C0C0, 0xC56A31, 0xF0F0F0};
  uint32_t i = Arg(c, 0);
  return i < 31 ? kColors[i] : 0;
}

HLE("user32.dll", SystemParametersInfoA, 4) {
  uint32_t action = Arg(c, 0), pv = Arg(c, 2);
  switch (action) {
    case 0x0030:  // SPI_GETWORKAREA
      Wr32(pv, 0);
      Wr32(pv + 4, 0);
      Wr32(pv + 8, uint32_t(g_display_w));
      Wr32(pv + 12, uint32_t(g_display_h));
      return 1;
    case 0x0003:  // SPI_GETBEEP
    case 0x0062:  // SPI_GETSCREENSAVERRUNNING
      if (pv) Wr32(pv, 0);
      return 1;
    case 0x0010:  // SPI_GETSCREENSAVEACTIVE
      if (pv) Wr32(pv, 0);
      return 1;
    case 0x0032:  // SPI_GETTOGGLEKEYS (struct: cbSize, dwFlags)
    case 0x0034:  // SPI_GETSTICKYKEYS
    case 0x003A:  // SPI_GETFILTERKEYS
      if (pv) Wr32(pv + 4, 0);
      return 1;
    case 0x0011:  // SPI_SETSCREENSAVEACTIVE
    case 0x0033:  // SPI_SETTOGGLEKEYS
    case 0x0035:  // SPI_SETSTICKYKEYS
    case 0x003B:  // SPI_SETFILTERKEYS
      return 1;
    case 0x0068:  // SPI_GETWHEELSCROLLLINES
      if (pv) Wr32(pv, 3);
      return 1;
    default:
      WarnF("SystemParametersInfoA(%X): unsupported, returning FALSE\n", action);
      return 0;
  }
}

HLE("user32.dll", RegisterWindowMessageA, 1) {
  static std::mutex mu;
  static std::map<std::string, uint32_t> atoms;
  static uint32_t next = 0xC000;
  std::lock_guard<std::mutex> l(mu);
  std::string n = Lower(GStr(Arg(c, 0)));
  auto it = atoms.find(n);
  if (it != atoms.end()) return it->second;
  return atoms[n] = next++;
}

// Cursors and icons: opaque handles; the visible cursor is SDL's.
HLE("user32.dll", LoadCursorA, 2) { return 0x00010000u | (Arg(c, 1) & 0xFFFF); }
HLE("user32.dll", LoadIconA, 2) { return 0x00020000u | (Arg(c, 1) & 0xFFFF); }

// Images are opaque handles too: nothing is drawn through GDI. A file
// (LR_LOADFROMFILE) must exist, as the game may check for the splash
// bitmap; resource bitmaps are not read.
HLE("user32.dll", LoadImageA, 6) {
  uint32_t name = Arg(c, 1), type = Arg(c, 2), flags = Arg(c, 5);
  static std::atomic<uint32_t> next{0x00030000};
  if (flags & 0x10) {  // LR_LOADFROMFILE
    struct stat st;
    std::string path = HostPath(GStr(name));
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
      SetLastError(2);  // ERROR_FILE_NOT_FOUND
      return 0;
    }
    return next += 0x10;
  }
  if (type == 1) return 0x00020000u | (name & 0xFFFF);  // IMAGE_ICON
  if (type == 2) return 0x00010000u | (name & 0xFFFF);  // IMAGE_CURSOR
  WarnF("LoadImageA: bitmap resource %X not read; returning a handle\n", name);
  return next += 0x10;
}
HLE("user32.dll", SetCursor, 1) {
  static uint32_t cur = 0;
  uint32_t old = cur;
  cur = Arg(c, 0);
  return old;
}

HLE("user32.dll", MessageBeep, 1) { return 1; }

// MessageBox(hwnd, text, caption, type) as a host dialog with the Win32
// buttons, default button and icon; returns the ID of the button chosen.
// Background runs log it and take the default button.
uint32_t ShowMessageBox(uint32_t owner, const std::string& text,
                        const std::string& caption, uint32_t type) {
  struct Button {
    uint32_t id;
    const char* label;
  };
  constexpr uint32_t IDOK = 1, IDCANCEL = 2, IDABORT = 3, IDRETRY = 4,
                     IDIGNORE = 5, IDYES = 6, IDNO = 7, IDTRYAGAIN = 10,
                     IDCONTINUE = 11;
  std::vector<Button> buttons;
  switch (type & 0xF) {
    case 1: buttons = {{IDOK, "OK"}, {IDCANCEL, "Cancel"}}; break;
    case 2: buttons = {{IDABORT, "Abort"}, {IDRETRY, "Retry"}, {IDIGNORE, "Ignore"}}; break;
    case 3: buttons = {{IDYES, "Yes"}, {IDNO, "No"}, {IDCANCEL, "Cancel"}}; break;
    case 4: buttons = {{IDYES, "Yes"}, {IDNO, "No"}}; break;
    case 5: buttons = {{IDRETRY, "Retry"}, {IDCANCEL, "Cancel"}}; break;
    case 6: buttons = {{IDCANCEL, "Cancel"}, {IDTRYAGAIN, "Try Again"}, {IDCONTINUE, "Continue"}}; break;
    default: buttons = {{IDOK, "OK"}}; break;
  }
  // MB_DEFBUTTON2..4 pick a later button.
  size_t def = std::min<size_t>((type >> 8) & 0xF, buttons.size() - 1);
  std::fprintf(stderr, "MessageBox: %s: %s\n", caption.c_str(), text.c_str());
  if (Background()) {
    std::fprintf(stderr, "MessageBox: answered \"%s\" (background run)\n",
                 buttons[def].label);
    return buttons[def].id;
  }
  std::vector<SDL_MessageBoxButtonData> sdl;
  for (size_t i = 0; i < buttons.size(); i++) {
    SDL_MessageBoxButtonFlags f = 0;
    if (i == def) f |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    // Escape (and closing the box) mean Cancel, or OK when it is alone.
    if (buttons[i].id == IDCANCEL || buttons.size() == 1)
      f |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    sdl.push_back({f, int(buttons[i].id), buttons[i].label});
  }
  SDL_MessageBoxFlags flags = SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
  switch (type & 0xF0) {
    case 0x10: flags |= SDL_MESSAGEBOX_ERROR; break;        // MB_ICONERROR
    case 0x30: flags |= SDL_MESSAGEBOX_WARNING; break;      // MB_ICONWARNING
    case 0x20:                                              // MB_ICONQUESTION
    case 0x40: flags |= SDL_MESSAGEBOX_INFORMATION; break;  // MB_ICONINFORMATION
  }
  SDL_MessageBoxData data{};
  data.flags = flags;
  data.window = owner ? SdlWindowFor(owner) : nullptr;
  const std::string title_utf8 = AnsiToUtf8(caption), text_utf8 = AnsiToUtf8(text);
  data.title = title_utf8.c_str();
  data.message = text_utf8.c_str();
  data.numbuttons = int(sdl.size());
  data.buttons = sdl.data();
  int chosen = -1;
  if (!SDL_ShowMessageBox(&data, &chosen)) {
    WarnF("MessageBox: cannot show a dialog (%s); answering \"%s\"\n",
          SDL_GetError(), buttons[def].label);
    return buttons[def].id;
  }
  // Closed without a button (no Cancel to map it to): the default.
  return chosen < 0 ? buttons[def].id : uint32_t(chosen);
}

HLE("user32.dll", MessageBoxA, 4) {
  return ShowMessageBox(Arg(c, 0), GStr(Arg(c, 1)), GStr(Arg(c, 2)), Arg(c, 3));
}

HLE("user32.dll", MessageBoxW, 4) {
  return ShowMessageBox(Arg(c, 0), Narrow(GWStr(Arg(c, 1))),
                        Narrow(GWStr(Arg(c, 2))), Arg(c, 3));
}

// ---- wsprintfA ------------------------------------------------------------------
// cdecl varargs: wsprintfA(buf, fmt, ...). Supports what user32 does:
// flags -#0, width, precision, h/l/ll prefixes, c C d i u x X s S %.
// No floating point (user32's wsprintf has none).

HLE("user32.dll", wsprintfA, kCdecl) {
  uint32_t out = Arg(c, 0);
  std::string fmt = GStr(Arg(c, 1));
  int argi = 2;
  auto next = [&]() { return Arg(c, argi++); };
  std::string r;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') {
      r += fmt[i];
      continue;
    }
    std::string spec = "%";
    i++;
    while (i < fmt.size() && std::strchr("-#0 +", fmt[i])) spec += fmt[i++];
    while (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) spec += fmt[i++];
    if (i < fmt.size() && fmt[i] == '.') {
      spec += fmt[i++];
      while (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) spec += fmt[i++];
    }
    bool wide = false, longlong = false;
    while (i < fmt.size() && std::strchr("hlwI", fmt[i])) {
      if (fmt[i] == 'l' || fmt[i] == 'w') wide = true;
      if (fmt[i] == 'I' && fmt.compare(i, 3, "I64") == 0) {
        longlong = true;
        i += 2;
      }
      if (fmt[i] == 'l' && i + 1 < fmt.size() && fmt[i + 1] == 'l') {
        longlong = true;
        i++;
      }
      i++;
    }
    if (i >= fmt.size()) break;
    char conv = fmt[i];
    char buf[512];
    switch (conv) {
      case '%': r += '%'; break;
      case 'd': case 'i': case 'u': case 'x': case 'X': {
        if (longlong) {
          uint64_t lo = next(), hi = next();
          uint64_t v = (hi << 32) | lo;
          spec += std::string("ll") + conv;
          std::snprintf(buf, sizeof buf, spec.c_str(), v);
        } else {
          uint32_t v = next();
          spec += conv;
          if (conv == 'd' || conv == 'i')
            std::snprintf(buf, sizeof buf, spec.c_str(), int32_t(v));
          else
            std::snprintf(buf, sizeof buf, spec.c_str(), v);
        }
        r += buf;
        break;
      }
      case 'c': case 'C': {
        uint32_t v = next();
        r += char(v);
        break;
      }
      case 's': case 'S': {
        uint32_t p = next();
        std::string s = !p ? "(null)" : (wide || conv == 'S') ? Narrow(GWStr(p)) : GStr(p);
        spec += 's';
        std::snprintf(buf, sizeof buf, spec.c_str(), s.c_str());
        r += buf;
        break;
      }
      default:
        r += spec + conv;
        break;
    }
  }
  if (r.size() > 1023) r.resize(1023);  // wsprintf's documented limit
  PutStr(out, r);
  return uint32_t(r.size());
}

}  // namespace
}  // namespace hle
}  // namespace recomp
