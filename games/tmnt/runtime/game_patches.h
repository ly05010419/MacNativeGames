#pragma once
#include "runtime.h"

/* No game-code patches are known to be safe for this TMNT build. */
void runtime_game_patches(Module *main_module);
