/* This game's code patches, applied to the rewritten text before it is sealed (loader.c). */
#pragma once
#include <stdint.h>
typedef struct { uint64_t offset; uint32_t original, replacement; const char *why; } GamePatch;
extern const GamePatch runtime_game_patch_list[];
extern const unsigned runtime_game_patch_count;
/* Checks every original word first: a different build stops the start instead of being patched blindly. */
void runtime_game_patches(uint8_t *base, uint64_t text_end);
