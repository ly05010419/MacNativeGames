// The launcher handshake of Red Alert 2 1.006 and Yuri's Revenge 1.001,
// answered by this process (handshake.cpp).
#pragma once

namespace recomp {
struct Ctx;
}

namespace ra2 {

// Creates the launcher's named mutex and event and answers the game's
// notification with a mapping holding `check_line`, the text the game
// compares (Red Alert 2: the copyright line; Yuri's Revenge:
// "UIDATA,3DDATA,MAPS"). Before the game's entry point, on its main thread.
void StartLauncherHandshake(recomp::Ctx& c, const char* check_line);

}  // namespace ra2
