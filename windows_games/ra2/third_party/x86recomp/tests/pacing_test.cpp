// FramePacer (runtime/pacing.h): the average frame rate is the one asked
// for, frames start close to their deadlines, waiting does not spin, and
// a late frame does not cause a burst.
// Timing on a shared host: the bounds leave room for scheduling noise.

#include <time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "runtime/pacing.h"
#include "runtime/platform.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
  }
}

uint64_t CpuNs() {
  timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return uint64_t(ts.tv_sec) * 1000000000u + uint64_t(ts.tv_nsec);
}

// One second of frames at fps with "work" (a sleep) of a varying part of
// the period: mean period, worst lateness behind the grid, CPU share.
void Rate(double fps) {
  recomp::FramePacer p;
  const int n = int(fps);
  const double period = 1e9 / fps;
  std::vector<uint64_t> start(n);
  const uint64_t cpu0 = CpuNs();
  for (int i = 0; i < n; i++) {
    p.Wait(fps);
    start[i] = recomp::platform::MonotonicNs();
    std::this_thread::sleep_for(std::chrono::nanoseconds(int64_t(period * 0.1 * (i % 6))));
  }
  const double cpu = double(CpuNs() - cpu0);
  const double wall = double(start[n - 1] - start[0]);
  const double mean = wall / (n - 1);
  std::vector<double> lateness(n);
  for (int i = 0; i < n; i++) lateness[i] = double(start[i] - start[0]) - i * period;
  const double late = *std::max_element(lateness.begin(), lateness.end());
  std::nth_element(lateness.begin(), lateness.begin() + n / 2, lateness.end());
  const double median = lateness[n / 2];
  std::printf("%.0f fps: mean period %.4f ms (%+.3f%%), lateness median %.3f max %.3f ms, "
              "CPU %.1f%%\n",
              fps, mean / 1e6, (mean / period - 1) * 100, median / 1e6, late / 1e6,
              cpu / wall * 100);
  Check(std::fabs(mean / period - 1) < 0.01, "mean period within 1%");
  // Wake-ups land some tens of microseconds late; timer coalescing (a
  // sleep with leeway) made it milliseconds.
  Check(median < 0.5e6, "median lateness under 0.5 ms");
  Check(cpu / wall < 0.25, "waiting does not spin");
}

// A frame three periods long: the next frame starts at once, the one
// after a full period later (no catch-up burst).
void Late() {
  recomp::FramePacer p;
  const double fps = 200, period = 1e9 / fps;
  for (int i = 0; i < 5; i++) p.Wait(fps);
  std::this_thread::sleep_for(std::chrono::nanoseconds(int64_t(period * 3)));
  const uint64_t t0 = recomp::platform::MonotonicNs();
  p.Wait(fps);
  const uint64_t t1 = recomp::platform::MonotonicNs();
  p.Wait(fps);
  const uint64_t t2 = recomp::platform::MonotonicNs();
  std::printf("after a late frame: %.3f ms, then %.3f ms\n", (t1 - t0) / 1e6, (t2 - t1) / 1e6);
  Check(t1 - t0 < period * 0.5, "a late frame is followed at once");
  Check(t2 - t1 > period * 0.9, "no burst after a late frame");
}

}  // namespace

int main() {
  Rate(144);
  Rate(240);
  Late();
  if (g_failures) return 1;
  std::printf("ok\n");
  return 0;
}
