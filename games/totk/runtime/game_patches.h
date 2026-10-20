#pragma once
#include "runtime.h"

/* TOTK version-specific game-code patches, applied to the loaded main module after relocation and before its text is sealed.
   Every site is checked against the original instruction word first; a mismatch stops the boot. */
void runtime_game_patches(Module *main_module);

/* Settings picture mode (runtime/settings.h): 0 off / 1080p, 1 MetalFX 2K, 2 native 2K. The deterministic counterpart
   of key 3's cycle; -1 when no 2K-capable start exists, -2 while the display layout or a previous switch is pending. */
int runtime_output_available(void);
int runtime_output_mode_get(void);   /* -1 when unavailable, otherwise the live OFF/MFX/NATIVE mode */
int runtime_output_mode_set(int mode);   /* 0 on success, -1 unavailable, -2 pending */
