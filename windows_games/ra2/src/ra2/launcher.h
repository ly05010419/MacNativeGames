// What the retail launcher (Ra2.exe) does for GAME.EXE, done by this process
// before the game's entry point. See launcher.cpp.
#pragma once

namespace recomp {
struct Ctx;
}

namespace ra2 {

// GameConfig::before_entry.
void BeforeEntry(recomp::Ctx& c);

}  // namespace ra2
