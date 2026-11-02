// Frame pacing; see pacing.h.

#include "runtime/pacing.h"

#include "runtime/platform.h"

namespace recomp {

void FramePacer::Wait(double fps) {
  if (!(fps > 0)) {
    next_ = 0;
    return;
  }
  const uint64_t period = uint64_t(1e9 / fps + 0.5);
  const uint64_t now = platform::MonotonicNs();
  if (!next_ || period != period_ || now >= next_ + period)
    next_ = now;
  else if (now < next_)
    platform::SleepUntilNs(next_);
  period_ = period;
  next_ += period;
}

}  // namespace recomp
