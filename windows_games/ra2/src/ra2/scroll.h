// The on-screen stick moving the battlefield view (scroll.cpp).
#pragma once

namespace recomp {
struct Ctx;
}

namespace ra2 {

// From the replacement of the tactical view's per-frame update (hooks.cpp,
// src/yr/hooks.cpp), before the game's own code runs: moves the view's
// centre by the stick, through the game's clamp and corner recomputing.
void ScrollByStick(recomp::Ctx& c);

}  // namespace ra2
