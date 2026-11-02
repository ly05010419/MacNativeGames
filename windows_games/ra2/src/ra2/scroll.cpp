// The on-screen stick moves the battlefield view directly (no pointer):
// the tactical view's per-frame update (thiscall, called through its
// vtable; Red Alert 2 0x6A2DD0, Yuri's Revenge 0x6D2540, the same code;
// replaced in hooks.cpp and src/yr/hooks.cpp) first moves the view's
// centre by the stick, the way that update moves it toward a scroll target:
//
//   centre (+0xD64/+0xD68, screen pixels) -> the map-bounds clamp
//   (addresses.h, stdcall, one Point2D*; true when it clamped) -> the
//   centre and its copy (+0xD74/+0xD78) -> the corner's recomputing
//   (thiscall, +0xB0) -> the redraw flag (+0xD7D)
//
// and then runs as usual. The game's own state is never bypassed: the
// clamp, the corner and the redraw are the game's code.

#include "scroll.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "addresses.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/screen_controls.h"
#include "runtime/runtime.h"

namespace ra2 {
namespace {

// The view object's fields, the same in both games.
enum : uint32_t { kCentreX = 0xD64, kCentreY = 0xD68, kCopyX = 0xD74, kCopyY = 0xD78,
                  kRedraw = 0xD7D };

// Full deflection: screen pixels per second (the game's edge scrolling
// moves about 1600). The stick's dead zone is its own.
constexpr float kMaxSpeed = 1600.0f;
constexpr float kDeadZone = 0.15f;

using Clock = std::chrono::steady_clock;

// RA2_SCROLL_LOG=1: the hook's calls and the stick once a second, and
// every move of the view.
bool LogOn() {
  static const bool on = [] {
    const char* e = std::getenv("RA2_SCROLL_LOG");
    return e && *e == '1';
  }();
  return on;
}

void LogCalls(bool held, float vx, float vy) {
  if (!LogOn()) return;
  static int calls = 0;
  static Clock::time_point t0 = Clock::now();
  calls++;
  if (Clock::now() - t0 < std::chrono::seconds(1)) return;
  std::fprintf(stderr, "ra2 scroll: %d calls/s, stick %s %.2f %.2f\n", calls,
               held ? "held" : "free", vx, vy);
  calls = 0;
  t0 = Clock::now();
}

}  // namespace

void ScrollByStick(recomp::Ctx& c) {
  static Clock::time_point last = Clock::now();
  static float rem_x = 0, rem_y = 0;
  static uint32_t point = 0;  // a guest Point2D for the clamp
  const Clock::time_point now = Clock::now();
  const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
  last = now;
  float vx, vy;
  const GameAddresses& a = Addresses();
  const uint32_t tac = recomp::Rd32(a.tactical_ptr);
  const bool held = recomp::StickDeflection(&vx, &vy);
  LogCalls(held, vx, vy);
  if (!held || c.ecx != tac || !tac) {
    rem_x = rem_y = 0;
    return;
  }
  const float len = std::hypot(vx, vy);
  if (len < kDeadZone) return;
  const float speed = kMaxSpeed * (len - kDeadZone) / (1 - kDeadZone) / len;
  rem_x += vx * speed * dt;
  rem_y += vy * speed * dt;
  const int dx = int(rem_x), dy = int(rem_y);
  rem_x -= float(dx);
  rem_y -= float(dy);
  if (!dx && !dy) return;

  if (!point) point = recomp::HeapAllocProcess(8, true);
  const recomp::Ctx saved = c;
  int32_t x = int32_t(recomp::Rd32(tac + kCentreX)) + dx;
  int32_t y = int32_t(recomp::Rd32(tac + kCentreY)) + dy;
  recomp::Wr32(point, uint32_t(x));
  recomp::Wr32(point + 4, uint32_t(y));
  const bool clamped = (recomp::CallGuest(c, a.clamp_to_map, {point}) & 0xFF) != 0;
  if (clamped && !recomp::Rd8(a.unclamped)) {
    x = int32_t(recomp::Rd32(point));
    y = int32_t(recomp::Rd32(point + 4));
  }
  recomp::Wr32(tac + kCentreX, uint32_t(x));
  recomp::Wr32(tac + kCopyX, uint32_t(x));
  recomp::Wr32(tac + kCentreY, uint32_t(y));
  recomp::Wr32(tac + kCopyY, uint32_t(y));
  c.ecx = tac;
  recomp::CallGuest(c, a.view_moved, {});
  recomp::Wr8(tac + kRedraw, 1);
  if (LogOn())
    std::fprintf(stderr, "ra2 scroll: centre -> %d,%d (clamped %d)\n", x, y, int(clamped));
  c = saved;
}

}  // namespace ra2
