#ifndef TOTK_METAL_HUD_H
#define TOTK_METAL_HUD_H

#include <stdlib.h>

/* The app launcher sets these before exec so Metal sees them at process start.
   The Mac frontend also uses them for direct development launches. Explicit
   environment settings win, including opting out of the HUD for measurements.
   Keep the game's existing monitors and add the Metal HUD at top-left so it
   does not cover the original top-right FPS panel. Per-frame logging is opt-in. */
static inline void runtime_metal_hud_defaults(void) {
    setenv("MTL_HUD_ENABLED", "1", 0);
    setenv("MTL_HUD_LOG_ENABLED", "0", 0);
    setenv("MTL_HUD_ALIGNMENT", "topleft", 0);
    setenv("TOTK_FPS_OVERLAY", "1", 0);
    setenv("TOTK_LOAD_OVERLAY", "1", 0);
}

#endif
