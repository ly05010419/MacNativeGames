// WinMM multimedia timers (timeSetEvent / timeKillEvent). As on Windows,
// one timer thread runs every timer: it calls the guest's TimeProc, or
// sets or pulses the event handle given in its place
// (TIME_CALLBACK_EVENT_SET / _PULSE). Periodic timers keep their phase;
// periods missed while the thread was busy are skipped, not caught up.

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t TIME_PERIODIC = 0x1, TIME_CALLBACK_EVENT_SET = 0x10,
                   TIME_CALLBACK_EVENT_PULSE = 0x20, TIME_KILL_SYNCHRONOUS = 0x100;
constexpr uint32_t TIMERR_NOERROR = 0, MMSYSERR_INVALPARAM = 11;

struct Timer {
  uint32_t period_ms = 0, proc = 0, user = 0, flags = 0;
  Clock::time_point next;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::map<uint32_t, Timer> g_timers;
uint32_t g_next_id = 1;
uint32_t g_running = 0;  // the timer whose action is running now
Thread* g_thread = nullptr;
std::thread::id g_host_id;

void Loop(Thread* t) {
  g_host_id = std::this_thread::get_id();
  std::unique_lock<std::mutex> l(g_mu);
  for (;;) {
    if (g_timers.empty()) {
      g_cv.wait(l);
      continue;
    }
    auto due = g_timers.begin();
    for (auto it = g_timers.begin(); it != g_timers.end(); ++it)
      if (it->second.next < due->second.next) due = it;
    if (Clock::now() < due->second.next) {
      g_cv.wait_until(l, due->second.next);
      continue;  // the timers may have changed
    }
    uint32_t id = due->first;
    Timer tm = due->second;
    if (tm.flags & TIME_PERIODIC) {
      auto period = std::chrono::milliseconds(tm.period_ms);
      Clock::time_point now = Clock::now();
      do due->second.next += period;
      while (due->second.next <= now);
    } else {
      g_timers.erase(due);
    }
    g_running = id;
    l.unlock();
    if (tm.flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE))
      SignalEventHandle(tm.proc, tm.flags & TIME_CALLBACK_EVENT_PULSE);
    else  // TimeProc(uTimerID, uMsg, dwUser, dw1, dw2)
      CallGuest(t->ctx, tm.proc, {id, 0, tm.user, 0, 0});
    l.lock();
    g_running = 0;
    g_cv.notify_all();
  }
}

// (uDelay, uResolution, lpTimeProc, dwUser, fuEvent)
HLE("winmm.dll", timeSetEvent, 5) {
  uint32_t delay = Arg(c, 0), proc = Arg(c, 2), flags = Arg(c, 4);
  if (!delay || !proc) return 0;
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_thread) {
    g_thread = CreateGuestThread(0x100000);
    Thread* t = g_thread;
    StartServiceThread(t, [t] { Loop(t); });
  }
  uint32_t id = g_next_id++;
  Timer& tm = g_timers[id];
  tm.period_ms = delay;
  tm.proc = proc;
  tm.user = Arg(c, 3);
  tm.flags = flags;
  tm.next = Clock::now() + std::chrono::milliseconds(delay);
  g_cv.notify_all();
  return id;
}

// (uTimerID): with TIME_KILL_SYNCHRONOUS, returns after a running action
// of the timer has finished (unless called from it).
HLE("winmm.dll", timeKillEvent, 1) {
  std::unique_lock<std::mutex> l(g_mu);
  auto it = g_timers.find(Arg(c, 0));
  bool running = g_running == Arg(c, 0);
  if (it == g_timers.end() && !running) return MMSYSERR_INVALPARAM;
  bool sync = it != g_timers.end() && (it->second.flags & TIME_KILL_SYNCHRONOUS);
  if (it != g_timers.end()) g_timers.erase(it);
  if (sync && std::this_thread::get_id() != g_host_id)
    g_cv.wait(l, [&] { return g_running != Arg(c, 0); });
  return TIMERR_NOERROR;
}

}  // namespace
}  // namespace recomp::hle
