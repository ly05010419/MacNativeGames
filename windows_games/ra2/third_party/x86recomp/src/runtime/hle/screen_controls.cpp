#include "runtime/hle/screen_controls.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "runtime/gfx/device.h"
#include "runtime/runtime.h"

namespace recomp {
namespace {

constexpr float kDeadZone = 0.3f;     // of the stick's radius
constexpr float kRadius = 0.17f;      // of the drawable's height
constexpr float kHitScale = 1.3f;     // the area a finger takes the stick in
constexpr float kKnobScale = 0.42f;   // knob radius, of the stick's
constexpr float kMargin = 0.03f;      // of the drawable's height
constexpr uint64_t kFpsPeriodMs = 500;
constexpr uint64_t kEdgeRepeatMs = 100;  // re-asserts the edge (a tap moved the pointer)
constexpr float kButtonScale = 0.38f;  // button radius, of the stick's
constexpr float kButtonRing = 1.85f;   // the buttons' arc: its radius, of the stick's
constexpr float kRowGap = 0.35f;       // a row's gaps (and before the first), of a button's radius
constexpr float kPressedScale = 0.9f;  // a button's size while held down
constexpr uint64_t kPressedShowMs = 120;  // it looks pressed at least this long (a quick tap)
constexpr uint64_t kStickyReleaseMs = 300;  // after the click or key: the game has read it
// The most from a tap's release to the next press for the two to be a
// double tap (the second acts as it lands).
constexpr uint64_t kDoubleTapMs = 450;
constexpr uint64_t kRepeatPressMs = 120;  // between a double tap's presses of its key
// Images: the FPS text, the stick, then two per button (idle, armed).
enum : int { kImageFps = 1, kImageBase = 2, kImageKnob = 3, kImageButtons = 4 };

// 5x7 glyphs, a row per byte (bit 4 leftmost).
struct Glyph {
  char ch;
  uint8_t rows[7];
};
constexpr Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
};

uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24;
}

// `text` in white glyphs `scale` pixels per dot on a translucent dark
// panel; *w, *h get the image's size.
std::vector<uint32_t> TextImage(const std::string& text, int scale, int* w, int* h) {
  const int pad = scale * 2, adv = 6 * scale;
  *w = int(text.size()) * adv - scale + 2 * pad;
  *h = 7 * scale + 2 * pad;
  std::vector<uint32_t> px(size_t(*w) * size_t(*h), Rgba(0, 0, 0, 140));
  for (size_t i = 0; i < text.size(); i++) {
    const Glyph* g = nullptr;
    for (const Glyph& f : kFont)
      if (f.ch == text[i]) g = &f;
    if (!g) continue;
    for (int row = 0; row < 7; row++)
      for (int col = 0; col < 5; col++) {
        if (!(g->rows[row] >> (4 - col) & 1)) continue;
        for (int y = 0; y < scale; y++)
          for (int x = 0; x < scale; x++) {
            const int px_x = pad + int(i) * adv + col * scale + x, px_y = pad + row * scale + y;
            px[size_t(px_y) * size_t(*w) + size_t(px_x)] = Rgba(255, 255, 255, 255);
          }
      }
  }
  return px;
}

// A disc of radius r in a (2r+2)-pixel square: `fill` inside, a `ring`
// of `ring_w` pixels at the edge; edges anti-aliased.
std::vector<uint32_t> DiscImage(float r, uint32_t fill, uint32_t ring, float ring_w, int* size) {
  *size = int(std::ceil(2 * r)) + 2;
  const float c = *size / 2.0f;
  std::vector<uint32_t> px(size_t(*size) * size_t(*size), 0);
  auto blend = [](uint32_t col, float cover) {
    const float a = float(col >> 24) * std::clamp(cover, 0.0f, 1.0f);
    return (col & 0x00FFFFFF) | uint32_t(std::lround(a)) << 24;
  };
  for (int y = 0; y < *size; y++)
    for (int x = 0; x < *size; x++) {
      const float d = std::hypot(x + 0.5f - c, y + 0.5f - c);
      const float inside = r - d + 0.5f;  // coverage of the disc
      if (inside <= 0) continue;
      const float in_ring = d - (r - ring_w) + 0.5f;  // coverage of the ring band
      const uint32_t col = in_ring >= 1 ? ring : in_ring <= 0 ? fill : ring;
      px[size_t(y) * size_t(*size) + size_t(x)] = blend(col, inside);
    }
  return px;
}

// Whether point (x, y), from a button's centre, is on `text` (glyphs of
// kFont, centred, 0.9 of the button's radius r tall).
float LabelCover(const std::string& text, float x, float y, float r) {
  const float dot = r * 0.9f / 7;  // seven rows across 0.9 r
  const float w = (float(text.size()) * 6 - 1) * dot, h = 7 * dot;
  const float lx = x + w / 2, ly = y + h / 2;
  if (lx < 0 || ly < 0 || lx >= w || ly >= h) return 0;
  const int col = int(lx / dot), row = int(ly / dot);
  const size_t ch = size_t(col / 6);
  if (ch >= text.size() || col % 6 == 5) return 0;
  for (const Glyph& f : kFont)
    if (f.ch == text[ch]) return (f.rows[row] >> (4 - col % 6) & 1) ? 1.0f : 0.0f;
  return 0;
}

// A round button: a dark disc with a white `icon`; red while `armed`.
std::vector<uint32_t> ButtonImage(const GameConfig::ScreenButton& cfg, float r, bool armed,
                                  int* size) {
  using Icon = GameConfig::ScreenButton::Icon;
  const Icon icon = cfg.icon;
  std::vector<uint32_t> px =
      DiscImage(r, armed ? Rgba(200, 30, 30, 200) : Rgba(0, 0, 0, 110),
                Rgba(255, 255, 255, armed ? 230 : 150), std::max(2.0f, r * 0.07f), size);
  const float c = *size / 2.0f, w = std::max(2.0f, r * 0.11f);
  // Coverage of a stroke of width w along segment a-b, or a ring.
  auto segment = [&](float px_, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float t = std::clamp(((px_ - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    return w / 2 - std::hypot(px_ - ax - t * dx, py - ay - t * dy) + 0.5f;
  };
  auto ring = [&](float px_, float py, float rr) {
    return w / 2 - std::fabs(std::hypot(px_ - c, py - c) - rr) + 0.5f;
  };
  const float a = r * 0.38f;
  for (int y = 0; y < *size; y++)
    for (int x = 0; x < *size; x++) {
      const float fx = x + 0.5f, fy = y + 0.5f;
      float cover;
      if (icon == Icon::Label) {
        // 4x4 samples a pixel: the glyphs' dots are not pixel-aligned.
        cover = 0;
        for (int sy = 0; sy < 4; sy++)
          for (int sx = 0; sx < 4; sx++)
            cover += LabelCover(cfg.label, x + (sx + 0.5f) / 4 - c, y + (sy + 0.5f) / 4 - c, r) / 16;
      } else if (icon == Icon::Cross) {
        cover = std::max(segment(fx, fy, c - a, c - a, c + a, c + a),
                         segment(fx, fy, c - a, c + a, c + a, c - a));
      } else if (icon == Icon::Deploy) {
        // Four arrows out from the middle, along the diagonals.
        cover = 0;
        const float in = r * 0.12f, out = r * 0.5f, head = r * 0.2f;
        for (const float sx : {-1.0f, 1.0f})
          for (const float sy : {-1.0f, 1.0f}) {
            const float tx = c + sx * out, ty = c + sy * out;
            cover = std::max({cover, segment(fx, fy, c + sx * in, c + sy * in, tx, ty),
                              segment(fx, fy, tx, ty, tx - sx * head, ty),
                              segment(fx, fy, tx, ty, tx, ty - sy * head)});
          }
      } else {  // Target: a ring and the cross hairs through it
        const float b = r * 0.55f, g = r * 0.16f;
        cover = std::max({ring(fx, fy, a), segment(fx, fy, c - b, c, c - g, c),
                          segment(fx, fy, c + g, c, c + b, c), segment(fx, fy, c, c - b, c, c - g),
                          segment(fx, fy, c, c + g, c, c + b)});
      }
      cover = std::clamp(cover, 0.0f, 1.0f);
      if (cover <= 0) continue;
      uint32_t& p = px[size_t(y) * size_t(*size) + size_t(x)];
      // White over what is there.
      const float ia = float(p >> 24) / 255.0f, oa = cover + ia * (1 - cover);
      auto ch = [&](int shift) {
        const float under = float((p >> shift) & 0xFF) * ia * (1 - cover);
        return uint32_t(std::lround((255.0f * cover + under) / std::max(oa, 1e-6f)));
      };
      p = ch(0) | ch(8) << 8 | ch(16) << 16 | uint32_t(std::lround(oa * 255)) << 24;
    }
  return px;
}

struct Button {
  float x = 0, y = 0;     // centre (drawable pixels)
  bool armed = false;     // StickyKey: holding its key
  uint64_t release_ms = 0;  // StickyKey: when to let go (0: not due)
  uint64_t end_ms = 0;       // Key: when its key's modifier is let go (0: not due)
  bool end_double = false;   // ... the double tap's
  uint64_t tap_ms = 0;       // Key with a double tap: a tap waiting for a second one
  uint64_t pressed_until = 0;  // looks pressed until then, after being let go
  uint64_t repeat_ms = 0;      // a double tap's next press of its key (0: none due)
  int repeats_left = 0;
};

struct State {
  StickEdgeFn edge = nullptr;
  // Layout (drawable pixels).
  int dw = 0, dh = 0;
  float cx = 0, cy = 0, r = 0;
  int base_size = 0, knob_size = 0;
  int fps_w = 0, fps_h = 0;
  // The stick: the pointer holding it, its deflection and direction.
  bool held = false;
  uint64_t holder = 0;
  float vx = 0, vy = 0;
  int dx = 0, dy = 0;
  uint64_t edge_ms = 0;
  // The buttons, and the pointer pressing one.
  ScreenButtonFn button = nullptr;
  std::vector<Button> buttons;
  float br = 0;
  int button_size = 0;
  bool pressing = false;
  bool press_done = false;  // the press acted already (the second of a double tap)
  uint64_t presser = 0;
  size_t pressed = 0;
  // Frame rate.
  uint64_t fps_t0 = 0, fps_frames0 = 0;
  int fps = -1;
  gfx::Device* dev = nullptr;  // the one the images were given to
} g;

bool StickOn() { return Game().touch_stick != GameConfig::Stick::None; }

// The stick and buttons are on screen now (GameConfig::controls_shown).
bool ControlsShown() { return StickOn() && (!Game().controls_shown || Game().controls_shown()); }

void SetArmed(size_t i, bool armed);
void SetDirection(int dx, int dy);

// Lets go of the stick and any button when the controls go away.
void DropControls() {
  if (g.held) {
    g.held = false;
    g.vx = g.vy = 0;
    SetDirection(0, 0);
  }
  g.pressing = false;
  for (size_t i = 0; i < g.buttons.size(); i++)
    if (g.buttons[i].armed) SetArmed(i, false);
}

bool EdgeScroll() { return Game().touch_stick == GameConfig::Stick::EdgeScroll; }

void SetDirection(int dx, int dy) {
  if (dx == g.dx && dy == g.dy) return;
  g.dx = dx;
  g.dy = dy;
  g.edge_ms = SDL_GetTicks();
  if (g.edge && EdgeScroll()) g.edge(dx, dy);
}

void Deflect(float x, float y) {
  float vx = (x - g.cx) / g.r, vy = (y - g.cy) / g.r;
  const float len = std::hypot(vx, vy);
  if (len > 1) vx /= len, vy /= len;
  g.vx = vx;
  g.vy = vy;
  int dx, dy;
  StickDirection(vx, vy, &dx, &dy);
  SetDirection(dx, dy);
}

// Rebuilds the images when the drawable's size changes (or the device).
void Layout(gfx::Device* dev, int dw, int dh) {
  if (dev == g.dev && dw == g.dw && dh == g.dh) return;
  g.dev = dev;
  g.dw = dw;
  g.dh = dh;
  g.fps = -1;  // re-upload the text
  if (!StickOn()) return;
  g.r = kRadius * float(dh);
  // The bottom left corner (Tick moves it inside the safe area).
  g.cx = kMargin * float(dh) + g.r;
  g.cy = float(dh) - kMargin * float(dh) - g.r;
  const std::vector<uint32_t> base =
      DiscImage(g.r, Rgba(255, 255, 255, 40), Rgba(255, 255, 255, 150), std::max(2.0f, g.r * 0.05f),
                &g.base_size);
  dev->SetOverlayImage(kImageBase, base.data(), g.base_size, g.base_size);
  const std::vector<uint32_t> knob = DiscImage(g.r * kKnobScale, Rgba(255, 255, 255, 170),
                                               Rgba(255, 255, 255, 220), 2.0f, &g.knob_size);
  dev->SetOverlayImage(kImageKnob, knob.data(), g.knob_size, g.knob_size);
  const auto& cfg = Game().screen_buttons;
  g.buttons.resize(cfg.size());
  g.br = g.r * kButtonScale;
  for (size_t i = 0; i < cfg.size(); i++)
    for (const bool armed : {false, true}) {
      const std::vector<uint32_t> img = ButtonImage(cfg[i], g.br, armed, &g.button_size);
      dev->SetOverlayImage(kImageButtons + int(2 * i) + (armed ? 1 : 0), img.data(), g.button_size,
                           g.button_size);
    }
}

// The buttons' places: on their arc around the stick, or in a row along
// the bottom, their centres level with the stick's bottom, centred on the
// screen unless that would reach the stick.
void PlaceButtons() {
  const auto& cfg = Game().screen_buttons;
  if (Game().button_layout == GameConfig::ButtonLayout::BottomRow) {
    const float y = g.cy + g.r, step = 2 * g.br + kRowGap * g.br;
    const size_t n = std::min(g.buttons.size(), cfg.size());
    const float row_w = n ? float(n) * 2 * g.br + float(n - 1) * kRowGap * g.br : 0;
    const float left = std::max((float(g.dw) - row_w) / 2, g.cx + g.r + kRowGap * g.br);
    float x = left + g.br;
    for (size_t i = 0; i < g.buttons.size() && i < cfg.size(); i++, x += step) {
      g.buttons[i].x = x;
      g.buttons[i].y = y;
    }
    return;
  }
  const float d = kButtonRing * g.r;
  for (size_t i = 0; i < g.buttons.size() && i < cfg.size(); i++) {
    const float a = cfg[i].angle * float(M_PI) / 180;
    g.buttons[i].x = g.cx + d * std::cos(a);
    g.buttons[i].y = g.cy - d * std::sin(a);
  }
}


void SetArmed(size_t i, bool armed) {
  Button& b = g.buttons[i];
  b.release_ms = 0;
  if (b.armed == armed) return;
  b.armed = armed;
  if (g.button) g.button(i, armed ? ButtonEvent::Arm : ButtonEvent::Disarm);
}

// Button i acts: a tap, or a double tap.
void Act(size_t i, bool twice) {
  const auto& cfg = Game().screen_buttons;
  if (!g.button) return;
  g.button(i, twice ? ButtonEvent::DoubleTap : ButtonEvent::Tap);
  if (cfg[i].action == GameConfig::ScreenButton::Action::Key) {
    g.buttons[i].end_ms = SDL_GetTicks() + kStickyReleaseMs;
    g.buttons[i].end_double = twice;
  }
}

// A tap on button i: at once, or (a Key with a double tap) once no second
// tap follows, or the second one.
void Tap(size_t i) {
  const auto& cfg = Game().screen_buttons;
  if (i >= cfg.size() || i >= g.buttons.size()) return;
  if (cfg[i].action == GameConfig::ScreenButton::Action::StickyKey) {
    SetArmed(i, !g.buttons[i].armed);
    return;
  }
  Button& b = g.buttons[i];
  if (cfg[i].action != GameConfig::ScreenButton::Action::Key || !cfg[i].double_vk) {
    Act(i, false);
  } else {
    b.tap_ms = std::max<uint64_t>(SDL_GetTicks(), 1);
  }
}

// <ID>_BUTTON_LOG=1: button presses and releases, timed.
void LogPress(size_t i, const char* what) {
  static const bool on = [] {
    const char* e = Env("BUTTON_LOG");
    return e && *e && *e != '0';
  }();
  if (on)
    std::fprintf(stderr, "screen button %zu: %s at %llu ms\n", i, what,
                 static_cast<unsigned long long>(SDL_GetTicks()));
}

}  // namespace

void StickDirection(float vx, float vy, int* dx, int* dy) {
  *dx = *dy = 0;
  if (std::hypot(vx, vy) < kDeadZone) return;
  // Sector of 45 degrees centred on each direction.
  const float a = std::atan2(vy, vx);  // y down
  const int s = int(std::lround(a / (float(M_PI) / 4))) & 7;
  static const int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
  static const int kDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
  *dx = kDx[s];
  *dy = kDy[s];
}

void SetStickEdgeHandler(StickEdgeFn fn) { g.edge = fn; }

void SetScreenButtonHandler(ScreenButtonFn fn) { g.button = fn; }

void ScreenButtonsClicked() {
  const uint64_t due = SDL_GetTicks() + kStickyReleaseMs;
  for (Button& b : g.buttons)
    if (b.armed && !b.release_ms) b.release_ms = due;
}

bool StickDeflection(float* vx, float* vy) {
  *vx = g.vx;
  *vy = g.vy;
  return g.held;
}

void ScreenControlsTick(SDL_Window* window) {
  if (!Game().show_fps && !StickOn()) return;
  gfx::Device* dev = gfx::PresentingDevice();
  if (!dev || !window) return;
  int dw = 0, dh = 0;
  SDL_GetWindowSizeInPixels(window, &dw, &dh);
  if (dw <= 0 || dh <= 0) return;
  Layout(dev, dw, dh);
  // Safe area (points) to pixels: keeps the stick off rounded corners.
  SDL_Rect safe;
  if (StickOn() && SDL_GetWindowSafeArea(window, &safe)) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    const float k = ww > 0 ? float(dw) / float(ww) : 1.0f;
    g.cx = float(safe.x) * k + kMargin * float(dh) + g.r;
    g.cy = float(safe.y + safe.h) * k - kMargin * float(dh) - g.r;
  }
  const uint64_t now = SDL_GetTicks();
  std::vector<gfx::OverlayItem> items;
  if (Game().show_fps) {
    if (!g.fps_t0) {
      g.fps_t0 = now;
      g.fps_frames0 = dev->FramesShown();
    } else if (now - g.fps_t0 >= kFpsPeriodMs) {
      const uint64_t frames = dev->FramesShown() - g.fps_frames0;
      const int fps = int(std::lround(double(frames) * 1000.0 / double(now - g.fps_t0)));
      g.fps_t0 = now;
      g.fps_frames0 = dev->FramesShown();
      // <ID>_FPS_LOG=1: each measurement on stderr too.
      static const bool log = [] {
        const char* e = Env("FPS_LOG");
        return e && *e && *e != '0';
      }();
      if (log) std::fprintf(stderr, "fps: %d\n", fps);
      if (fps != g.fps) {
        g.fps = fps;
        const int scale = std::max(2, int(std::lround(dh / 250.0)));
        const std::vector<uint32_t> img =
            TextImage(std::to_string(fps) + " FPS", scale, &g.fps_w, &g.fps_h);
        dev->SetOverlayImage(kImageFps, img.data(), g.fps_w, g.fps_h);
      }
    }
    if (g.fps >= 0) {
      const float m = kMargin * float(dh);
      items.push_back({kImageFps, m, m, float(g.fps_w), float(g.fps_h), 1.0f});
    }
  }
  if (!ControlsShown()) {
    DropControls();
  } else {
    PlaceButtons();
    const float bz = float(g.button_size);
    for (size_t i = 0; i < g.buttons.size(); i++) {
      Button& b = g.buttons[i];
      if (b.release_ms && now >= b.release_ms) SetArmed(i, false);
      if (b.tap_ms && now - b.tap_ms >= kDoubleTapMs) {
        b.tap_ms = 0;
        Act(i, false);
      }
      if (b.repeat_ms && now >= b.repeat_ms) {
        b.repeat_ms = --b.repeats_left > 0 ? now + kRepeatPressMs : 0;
        Act(i, true);
      }
      if (b.end_ms && now >= b.end_ms) {
        b.end_ms = 0;
        if (g.button) g.button(i, b.end_double ? ButtonEvent::DoubleTapEnd : ButtonEvent::TapEnd);
      }
      // Held down: lit and a little smaller, as a pressed button.
      const bool down = (g.pressing && g.pressed == i) || now < b.pressed_until;
      const float z = down ? bz * kPressedScale : bz;
      items.push_back({kImageButtons + int(2 * i) + (b.armed || down ? 1 : 0), b.x - z / 2,
                       b.y - z / 2, z, z, 1.0f});
    }
    const float bs = float(g.base_size), ks = float(g.knob_size);
    items.push_back({kImageBase, g.cx - bs / 2, g.cy - bs / 2, bs, bs, 1.0f});
    const float kx = g.cx + g.vx * g.r * (1 - kKnobScale), ky = g.cy + g.vy * g.r * (1 - kKnobScale);
    items.push_back({kImageKnob, kx - ks / 2, ky - ks / 2, ks, ks, 1.0f});
    if ((g.dx || g.dy) && now - g.edge_ms >= kEdgeRepeatMs && g.edge && EdgeScroll()) {
      g.edge_ms = now;
      g.edge(g.dx, g.dy);
    }
  }
  dev->SetOverlayItems(std::move(items));
}

bool ScreenControlsDown(uint64_t id, float x, float y) {
  if (!ControlsShown() || g.r <= 0) return false;
  if (!g.pressing)
    for (size_t i = 0; i < g.buttons.size(); i++)
      if (std::hypot(x - g.buttons[i].x, y - g.buttons[i].y) <= g.br * kHitScale) {
        g.pressing = true;
        g.presser = id;
        g.pressed = i;
        g.press_done = false;
        LogPress(i, "down");
        // The second tap of a double tap acts as it lands.
        Button& b = g.buttons[i];
        if (b.tap_ms && SDL_GetTicks() - b.tap_ms < kDoubleTapMs) {
          b.tap_ms = 0;
          g.press_done = true;
          Act(i, true);
          b.repeats_left = std::max(0, int(Game().screen_buttons[i].double_presses) - 1);
          if (b.repeats_left) b.repeat_ms = SDL_GetTicks() + kRepeatPressMs;
        }
        return true;
      }
  if (g.held) return false;
  if (std::hypot(x - g.cx, y - g.cy) > g.r * kHitScale) return false;
  g.held = true;
  g.holder = id;
  Deflect(x, y);
  return true;
}

bool ScreenControlsMove(uint64_t id, float x, float y) {
  if (g.pressing && id == g.presser) return true;
  if (!g.held || id != g.holder) return false;
  Deflect(x, y);
  return true;
}

bool ScreenControlsUp(uint64_t id) {
  if (g.pressing && id == g.presser) {
    g.pressing = false;
    if (g.pressed < g.buttons.size()) g.buttons[g.pressed].pressed_until = SDL_GetTicks() + kPressedShowMs;
    LogPress(g.pressed, "up");
    if (!g.press_done) Tap(g.pressed);
    return true;
  }
  if (!g.held || id != g.holder) return false;
  g.held = false;
  g.vx = g.vy = 0;
  SetDirection(0, 0);
  return true;
}

}  // namespace recomp
