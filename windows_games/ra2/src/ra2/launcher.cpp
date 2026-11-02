// What the retail launcher (Ra2.exe) does for GAME.EXE, done by this
// process before the game's entry point: its handshake (handshake.cpp,
// with Red Alert 2's copyright line), and this project's settings.

#include "launcher.h"
#include "devtools.h"
#include "handshake.h"
#include "resolution.h"

#include <cstdint>

#include "runtime/hle/kernel32_state.h"
#include "runtime/runtime.h"

namespace ra2 {
namespace {

// The text GAME.EXE checks in the launcher's mapping (handshake.cpp).
constexpr char kLauncherLine[] = "(c) 2000 Electronic Arts, Inc. All Rights Reserved";

// The startup movie: the game plays WESTLOGO(.BIK) every time it starts
// (0x5126xx, through Play_Movie), skipping it when no such file is found.
// A touch screen cannot skip it either, and the project leaves movies out:
// the name is changed in memory to one no install has, so the game finds
// no file and goes on, as it does for installs without the movie.
constexpr uint32_t kStartupMovieName = 0x7DB604;  // "WESTLOGO"

void SkipStartupMovie() {
  const char want[] = "WESTLOGO";
  for (uint32_t i = 0; i < sizeof want - 1; i++)
    if (recomp::Rd8(kStartupMovieName + i) != uint8_t(want[i])) return;  // not this build
  recomp::Wr8(kStartupMovieName + 7, '_');
}

}  // namespace

void BeforeEntry(recomp::Ctx& c) {
  ApplyResolution();
  SkipStartupMovie();
  StartDevTools();
  StartLauncherHandshake(c, kLauncherLine);
}

}  // namespace ra2
