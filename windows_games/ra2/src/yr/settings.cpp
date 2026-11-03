// Yuri's Revenge's settings, applied before the game starts.

#include "settings.h"

#include <cstdint>

#include "../ra2/game_settings.h"
#include "../ra2/resolution.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/runtime.h"

namespace yr {
namespace {

std::string g_spec = "auto";

// The startup movie: the game plays EA_WWLOGO(.BIK) every time it starts
// (the name at 0x825F20, used once, at 0x52C5E2) and skips it when no such
// file is found. A touch screen cannot skip it either, and the project
// leaves movies out: the name is changed in memory to one no install has.
constexpr uint32_t kStartupMovieName = 0x825F20;  // "EA_WWLOGO"

void SkipStartupMovie() {
  const char want[] = "EA_WWLOGO";
  for (uint32_t i = 0; i < sizeof want - 1; i++)
    if (recomp::Rd8(kStartupMovieName + i) != uint8_t(want[i])) return;  // not this build
  recomp::Wr8(kStartupMovieName + 8, '_');
}

}  // namespace

void SetResolution(const std::string& spec) { g_spec = spec; }

void ApplySettings() {
  ra2::ApplyGameSettings("RA2MD.INI", g_spec, "yr", ra2::AdjustScreen);
  SkipStartupMovie();
}

}  // namespace yr
