// The games this project runs, each started by its RunGame (its main on
// the Mac; on iOS, after the player picks it, src/app/chooser.cpp).
// argv as main's. Registers the game's own lifted code first: an app that
// holds both games has both tables (Yuri's Revenge's renamed, CMakeLists).
#pragma once

namespace ra2 {
int RunGame(int argc, char** argv);  // src/ra2/main.cpp
}

namespace yr {
int RunGame(int argc, char** argv);  // src/yr/main.cpp
}
