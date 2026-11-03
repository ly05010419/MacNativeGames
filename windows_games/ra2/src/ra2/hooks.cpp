// Red Alert 2's functions the project replaces (config/game.manifest:
// replace), each running the shared code with the game's own lifted code:
//   0x540960 the battlefield's frame limiter (frame_pacing.h);
//   0x67B910 the sidebar's layout (sidebar.h);
//   0x6A2DD0 the tactical view's per-frame update (scroll.h).
// Yuri's Revenge's are in src/yr/hooks.cpp.

#include "frame_pacing.h"
#include "scroll.h"
#include "sidebar.h"
#include "runtime/runtime.h"

namespace recomp::gen {

void f_00540960_lifted(Ctx& c);
void f_0067B910_lifted(Ctx& c);
void f_006A2DD0_lifted(Ctx& c);

void f_00540960(Ctx& c) { ra2::PacedFrameLimiter(c, f_00540960_lifted); }

void f_0067B910(Ctx& c) { ra2::LayoutSidebar(c, f_0067B910_lifted); }

void f_006A2DD0(Ctx& c) {
  ra2::ScrollByStick(c);
  f_006A2DD0_lifted(c);
}

}  // namespace recomp::gen
