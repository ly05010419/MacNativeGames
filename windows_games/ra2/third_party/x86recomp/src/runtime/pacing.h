#pragma once
// Frame pacing for game patches that replace a busy-waiting frame limiter
// (ARCHITECTURE.md, "Frame pacing").

#include <cstdint>

namespace recomp {

// Paces a loop to a frame rate on the host's monotonic clock. Wait()
// sleeps, without spinning, until the next frame's start. Frame starts
// lie on a grid of exact periods (not rounded to milliseconds), so
// wake-up latency does not add up and the average rate is the one asked
// for. A frame that ends late is followed by a shorter wait; one that is
// more than a whole period late moves the grid instead of being followed
// by a burst of frames.
class FramePacer {
 public:
  // Call at the start of every frame. fps <= 0: no limit (and the grid
  // starts again at the next limited frame).
  void Wait(double fps);

 private:
  uint64_t next_ = 0;    // start of the next frame (MonotonicNs), 0: none
  uint64_t period_ = 0;  // ns
};

}  // namespace recomp
