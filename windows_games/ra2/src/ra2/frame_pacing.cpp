// Battlefield frames shown at an even rate. The game's frame limiter
// (Red Alert 2: 0x540960, hooks.cpp; Yuri's Revenge: 0x55E160,
// src/yr/hooks.cpp; no arguments) is called once a frame by the
// battlefield's main loop (0x53FBD0; 0x55D360) when the frame has been
// drawn; it then waits for the frame timer (single-player: 16 ms ticks of
// timeGetTime() >> 4, 0x839998 of them a frame, the game speed; observed
// 63 frames a second at the skirmish default). The game pumps messages at
// irregular points of its loop, so frames shown at pumps came 16 to 64 ms
// apart with gaps of hundreds of ms while the loop itself ran evenly.
// The hook tells the runtime each frame has ended (recomp::GameFrameEnd)
// and shows those that keep the rate under the cap: at the default 60,
// every 16 ms frame (62.5 a second); at 30, every second one, 31.8 ms
// apart.
//
// RA2_FRAME_LOG=1: each second, how many frames, how long the game worked
// between limiter calls, how long the limiter waited and how many frames
// were shown.

#include "frame_pacing.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "runtime/hle/user32_window.h"
#include "runtime/runtime.h"

namespace ra2 {
namespace {

using Clock = std::chrono::steady_clock;

int g_cap = 60;

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

bool LogOn() {
  static const bool on = [] {
    const char* e = std::getenv("RA2_FRAME_LOG");
    return e && *e == '1';
  }();
  return on;
}

// Whether the frame ending now is shown: the time since the last shown
// frame reaches the cap's interval, less half a frame. Frames come on a
// 16 ms grid, so this shows every Nth frame, N the nearest whole number of
// frames to the interval (16 ms frames at 30: every second one). Nothing
// is carried over from one shown frame to the next: a carried remainder
// (31.8 against 33.3 ms) would add up to an extra frame now and then.
bool ShowThisFrame(Clock::time_point now) {
  static Clock::time_point last{};
  static double since_shown_ms = 0;
  const double dt = last == Clock::time_point{} ? 0 : Ms(now - last);
  last = now;
  if (g_cap <= 0) return true;
  const double interval = 1000.0 / g_cap;
  since_shown_ms += dt;
  if (since_shown_ms < interval - std::min(dt, interval) / 2) return false;
  since_shown_ms = 0;
  return true;
}

struct Stats {
  Clock::time_point second{}, last_exit{};
  std::vector<double> between, waited;
  int shown = 0;
};

void Log(Stats& s, Clock::time_point entry, Clock::time_point exit, bool shown) {
  if (s.last_exit != Clock::time_point{}) s.between.push_back(Ms(entry - s.last_exit));
  s.waited.push_back(Ms(exit - entry));
  s.shown += shown;
  s.last_exit = exit;
  if (s.second == Clock::time_point{}) s.second = exit;
  if (exit - s.second < std::chrono::seconds(1)) return;
  auto spread = [](std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? std::make_pair(0.0, 0.0) : std::make_pair(v[v.size() / 2], v.back());
  };
  const auto [b_med, b_max] = spread(s.between);
  const auto [w_med, w_max] = spread(s.waited);
  std::fprintf(stderr,
               "ra2 frames: %zu/s, %d shown; work between limiter calls ms median %.1f max "
               "%.1f; limiter waited ms median %.1f max %.1f\n",
               s.waited.size(), s.shown, b_med, b_max, w_med, w_max);
  s.between.clear();
  s.waited.clear();
  s.shown = 0;
  s.second = exit;
}

}  // namespace

void SetFrameCap(int fps) { g_cap = std::max(0, fps); }
int FrameCap() { return g_cap; }

void PacedFrameLimiter(recomp::Ctx& c, void (*limiter)(recomp::Ctx&)) {
  const auto entry = Clock::now();
  const bool show = ShowThisFrame(entry);
  recomp::GameFrameEnd(show);
  limiter(c);
  if (LogOn()) {
    static Stats stats;
    Log(stats, entry, Clock::now(), show);
  }
}

}  // namespace ra2
