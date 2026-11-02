// The launcher handshake of Red Alert 2 1.006 and Yuri's Revenge 1.001,
// answered by this process.
//
// The retail games are started by their launchers (Ra2.exe, RA2MD.exe),
// never directly. GAME.EXE and gamemd.exe check for that
// (reference/FEASIBILITY.md §8, observed in their code; Yuri's Revenge at
// 0x49F6xx):
//   1. A named mutex the launcher creates and holds must exist
//      (CreateMutexA then GetLastError == ERROR_ALREADY_EXISTS); otherwise
//      the game exits at once.
//   2. The game opens the launcher's named event and sets it ("notify").
//   3. The launcher answers with a thread message 0xBEEF whose lParam is a
//      file mapping handle; the game maps it (MapViewOfFileEx) and compares
//      its text: Red Alert 2 the launcher's copyright line, Yuri's Revenge
//      "UIDATA,3DDATA,MAPS". Both use the same mutex and event names.
// This process is the launcher here: it creates the same named objects and
// answers the same way, through the runtime's Win32 objects (CallGuest on
// the imports), so the game's own code runs unchanged.

#include "handshake.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/log.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/runtime.h"

namespace ra2 {
namespace {

constexpr char kLauncherMutex[] = "48BC11BD-C4D7-466b-8A31-C6ABBAD47B3E";
constexpr char kNotifyEvent[] = "D6E7FC97-64F9-4d28-B52C-754EDF721C6F";
constexpr uint32_t kLauncherMessage = 0xBEEF;

constexpr uint32_t kInvalidHandle = 0xFFFFFFFF, kInfinite = 0xFFFFFFFF;
constexpr uint32_t kPageReadWrite = 0x04, kFileMapWrite = 0x02;
constexpr uint32_t kMappingSize = 0x1000;

uint32_t Import(recomp::Ctx& c, const char* dll, const char* fn,
                std::initializer_list<uint64_t> args) {
  return uint32_t(recomp::CallGuest(c, recomp::hle::ThunkFor(dll, fn), args));
}

// A NUL-terminated copy of s in guest memory (kept: the names live as long
// as the objects).
recomp::gaddr GuestString(const char* s) {
  const recomp::gaddr p = recomp::HeapAllocProcess(std::strlen(s) + 1, false);
  recomp::PutStr(p, s);
  return p;
}

// The launcher's answer: once the game sets the event, a mapping holding
// the line it checks, posted to the game's main thread.
void Answer(recomp::Thread* t, uint32_t event, uint32_t game_tid, const std::string& line) {
  recomp::Ctx& c = t->ctx;
  Import(c, "kernel32.dll", "WaitForSingleObject", {event, kInfinite});
  const uint32_t map = Import(c, "kernel32.dll", "CreateFileMappingA",
                              {kInvalidHandle, 0, kPageReadWrite, 0, kMappingSize, 0});
  if (!map) recomp::FatalF("launcher: CreateFileMappingA failed");
  const uint32_t view = Import(c, "kernel32.dll", "MapViewOfFile", {map, kFileMapWrite, 0, 0, 0});
  if (!view) recomp::FatalF("launcher: MapViewOfFile failed");
  recomp::PutStr(view, line.c_str());
  Import(c, "kernel32.dll", "UnmapViewOfFile", {view});
  Import(c, "user32.dll", "PostThreadMessageA", {game_tid, kLauncherMessage, 0, map});
}

}  // namespace

void StartLauncherHandshake(recomp::Ctx& c, const char* check_line) {
  const uint32_t game_tid = recomp::CurrentThread()->tid;
  // Held for the life of the process, as the launcher holds it while the
  // game runs.
  if (!Import(c, "kernel32.dll", "CreateMutexA", {0, 0, GuestString(kLauncherMutex)}))
    recomp::FatalF("launcher: CreateMutexA failed");
  const uint32_t event =
      Import(c, "kernel32.dll", "CreateEventA", {0, 0, 0, GuestString(kNotifyEvent)});
  if (!event) recomp::FatalF("launcher: CreateEventA failed");
  recomp::Thread* t = recomp::CreateGuestThread(0x100000);
  recomp::StartServiceThread(t, [t, event, game_tid, line = std::string(check_line)] {
    Answer(t, event, game_tid, line);
  });
}

}  // namespace ra2
