#pragma once
// Hang watchdog. The guest main thread reports a heartbeat each time it
// pumps host events, and one per presented frame; if either stops for 5 s
// (<ID>_WATCHDOG_SECS), every registered
// guest thread is asked (SIGUSR2) to write its host backtrace (lifted
// functions are named recomp::gen::f_<guest VA>) to a dump file in the
// per-user data directory and to stderr. The process keeps running.

#include <cstdint>

namespace recomp {

void StartWatchdog();
// Call on each host thread that runs guest code, once, at its start.
void RegisterGuestHostThread(const char* name, unsigned tid);
// Called by the main thread whenever it services the window system.
void WatchdogHeartbeat();
// Called on every presented frame. A minimized/inactive game presents
// nothing, so this only counts while the game is active.
void WatchdogFrame();
// The game was (de)activated; frames are only expected while active.
void WatchdogAppActive(bool active);

// The import a thread is executing (innermost) and its guest return
// address, maintained by hle::CallThunk; null outside imports. Exception
// unwinds through an import leave them stale, so dumps say "last".
extern thread_local const char* t_wd_hle_name;
extern thread_local uint64_t t_wd_hle_ret;

}  // namespace recomp
