// Battlefield frames shown at an even rate (frame_pacing.cpp).
#pragma once

namespace recomp {
struct Ctx;
}

namespace ra2 {

// The most battlefield frames shown a second (default 60); 0: every frame
// the game draws. Set before the game starts.
void SetFrameCap(int fps);
int FrameCap();

// The game's frame limiter, called through this from the replacement of
// the game's own (`limiter`: its lifted code): reports the frame's end to
// the runtime, showing it if it keeps the cap.
void PacedFrameLimiter(recomp::Ctx& c, void (*limiter)(recomp::Ctx&));

}  // namespace ra2
