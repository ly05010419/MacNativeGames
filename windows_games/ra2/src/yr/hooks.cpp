// Yuri's Revenge's functions the project replaces (config/yr.manifest:
// replace), the same code as Red Alert 2's at moved addresses (matched
// instruction by instruction; src/ra2/addresses.h), each running the shared
// code with the game's own lifted code:
//   0x55E160 the battlefield's frame limiter (Red Alert 2's 0x540960);
//   0x6A5130 the sidebar's layout (0x67B910, with a left-hand sidebar too);
//   0x6D2540 the tactical view's per-frame update (0x6A2DD0).
// Compiled in Yuri's Revenge's renamed namespace (CMakeLists.txt: gen is
// gen_yr here), so an app can hold both games' replacements.

#include "../ra2/frame_pacing.h"
#include "../ra2/scroll.h"
#include "../ra2/sidebar.h"
#include "runtime/runtime.h"

namespace recomp::gen {

void f_0055E160_lifted(Ctx& c);
void f_006A5130_lifted(Ctx& c);
void f_006D2540_lifted(Ctx& c);

void f_0055E160(Ctx& c) { ra2::PacedFrameLimiter(c, f_0055E160_lifted); }

void f_006A5130(Ctx& c) { ra2::LayoutSidebar(c, f_006A5130_lifted); }

void f_006D2540(Ctx& c) {
  ra2::ScrollByStick(c);
  f_006D2540_lifted(c);
}

}  // namespace recomp::gen
