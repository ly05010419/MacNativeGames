// The project's settings in the game's own INI file (game_settings.h).

#include "game_settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_window.h"
#include "runtime/runtime.h"

namespace ra2 {
namespace {

// The height a screen's shape is taken at. A phone's screen is small:
// there the game runs at 512 lines, so the sidebar and its build icons come
// out 1.5 times as large as at 768. The game fits its menus (800x600
// artwork) to such a height: their side panels drop the empty button
// slots and the artwork's bottom is cut, every control stays in view.
#ifdef SDL_PLATFORM_IOS
constexpr int kHeight = 512;
#else
constexpr int kHeight = 768;
#endif
// The desktop's size in pixels, landscape.
bool DesktopPixels(int* w, int* h) {
  recomp::EnsureSdlVideo();
  const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
  if (!m) return false;
  const float d = m->pixel_density > 0 ? m->pixel_density : 1.0f;
  const int a = int(std::lround(float(m->w) * d)), b = int(std::lround(float(m->h) * d));
  *w = std::max(a, b);
  *h = std::min(a, b);
  return *h > 0;
}

bool Parse(const std::string& spec, int* w, int* h) {
  if (spec == "auto") {
    int dw, dh;
    if (!DesktopPixels(&dw, &dh)) return false;
    *h = kHeight;
    // A multiple of 8, never narrower than the 4:3 mode at that height
    // nor than the menus.
    *w = std::max({kMenuWidth, kHeight * 4 / 3, int(std::lround(double(kHeight) * dw / dh / 8.0)) * 8});
    return true;
  }
  return std::sscanf(spec.c_str(), "%dx%d", w, h) == 2 && *w >= 640 && *h >= 480;
}

// Sets key=value in [section] of INI text with CRLF lines (added at the
// section's end, or with the section at the file's end, if missing).
std::string SetIniValue(const std::string& text, const std::string& section,
                        const std::string& key, const std::string& value) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(l);
  }
  const std::string header = "[" + section + "]", entry = key + "=" + value;
  auto is_section = [](const std::string& l) { return !l.empty() && l[0] == '['; };
  size_t i = 0;
  while (i < lines.size() && lines[i] != header) i++;
  if (i == lines.size()) {
    lines.push_back(header);
    lines.push_back(entry);
  } else {
    size_t end = i + 1;
    bool done = false;
    for (; end < lines.size() && !is_section(lines[end]); end++)
      if (lines[end].rfind(key + "=", 0) == 0) {
        lines[end] = entry;
        done = true;
      }
    if (!done) {
      while (end > i + 1 && lines[end - 1].empty()) end--;
      lines.insert(lines.begin() + std::ptrdiff_t(end), entry);
    }
  }
  std::string out;
  for (const std::string& l : lines) out += l + "\r\n";
  return out;
}

// Whether [section] of INI text has key= (any value).
bool HasIniValue(const std::string& text, const std::string& section, const std::string& key) {
  std::istringstream in(text);
  bool inside = false;
  for (std::string l; std::getline(in, l);) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!l.empty() && l[0] == '[')
      inside = l == "[" + section + "]";
    else if (inside && l.rfind(key + "=", 0) == 0)
      return true;
  }
  return false;
}

}  // namespace

void ApplyGameSettings(const std::string& ini_name, const std::string& spec, const char* tag,
                       void (*adjust)(int* w, int* h)) {
  const std::string ini = recomp::HostPath(recomp::Game().guest_dir + "\\" + ini_name,
                                           recomp::PathAccess::kUpdate);
  std::ifstream f(ini, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  f.close();
  std::string text = SetIniValue(ss.str(), "Intro", "Play", "no");
  if (!HasIniValue(text, "WinNativeGames", "DefaultsApplied")) {
    text = SetIniValue(text, "Options", "GameSpeed", "1");
    text = SetIniValue(text, "Skirmish", "GameSpeed", "1");
    text = SetIniValue(text, "WinNativeGames", "DefaultsApplied", "1");
  }
  int w = 0, h = 0;
  if (spec != "ini") {
    if (!Parse(spec, &w, &h)) {
      std::fprintf(stderr, "%s: resolution \"%s\" not understood (auto, WxH or ini)\n", tag,
                   spec.c_str());
      w = h = 0;
    } else {
      if (adjust) adjust(&w, &h);
      text = SetIniValue(text, "Video", "ScreenWidth", std::to_string(w));
      text = SetIniValue(text, "Video", "ScreenHeight", std::to_string(h));
    }
  }
  if (text == ss.str()) return;
  std::ofstream(ini, std::ios::binary | std::ios::trunc) << text;
  if (w) std::fprintf(stderr, "%s: screen %dx%d (%s)\n", tag, w, h, spec.c_str());
}

}  // namespace ra2
