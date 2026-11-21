#pragma once
/* What the ports built with this app (games/totk/ios/tools/build_port_ipa.py) have of its picture settings, so its
   screens show only what does something there. A port's picture: the mode (handheld 720p / docked 1080p), MetalFX from
   the canvas to the screen; no scene size, edge smoothing or 16:9 bars of ours, and its frame rate
   chosen before the game starts, if at all (picture_profile.h PP_GAME_ACNH / PP_GAME_ODYSSEY).
     SHELL_PORT        1 for a port (Animal Crossing, Super Mario Odyssey), 0 for the Zelda games and Bananza / Pokopia
     SHELL_FPS_CHOICE  1 where the picture's 30 / 60 does something: the Zelda games; Animal Crossing (its 60 FPS text) */
#if defined(GAME_ACNH) || defined(GAME_ODYSSEY)
#define SHELL_PORT 1
#else
#define SHELL_PORT 0
#endif
#if defined(GAME_ODYSSEY)
#define SHELL_FPS_CHOICE 0   /* it runs at its own 60 */
#else
#define SHELL_FPS_CHOICE 1
#endif
