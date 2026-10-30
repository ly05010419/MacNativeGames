/* What the build derives from the player's own game files (module tables, RomFS / add-on content / shared-font hashes) is
   read at start from the game data folder, not compiled into this program: the program holds nothing of the game, so a
   new build of it can replace the old one in an app without touching the game's files (update_runtime.py).
   The folder: TOTK_GAME_DATA, by default build/game-data/<version> under the current folder, where
   scripts/prepare_runtime.py, prepare_romfs_backend.py and prepare_shared_fonts.py write it; the app carries it at the
   same place (build_app.py step_app). */
#pragma once
#include <stddef.h>
#include <stdint.h>
/* <folder>/<name>; stops the start when it does not fit. */
void runtime_game_data_path(const char *name, char *out, size_t cap);
/* The value of KEY in <folder>/<file> ("KEY VALUE" lines); stops the start when the file or the key is missing. */
const char *runtime_game_text(const char *file, const char *key);
uint64_t runtime_game_number(const char *file, const char *key);
/* A SHA-256 value (64 lowercase hex digits). */
const char *runtime_game_sha256(const char *file, const char *key);
