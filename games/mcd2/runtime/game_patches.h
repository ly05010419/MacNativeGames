#pragma once
#include "runtime.h"

/* TOTK version-specific game-code patches, applied to the loaded main module after relocation and before its text is sealed.
   Every site is checked against the original instruction word first; a mismatch stops the boot. */
void runtime_game_patches(Module *main_module);
