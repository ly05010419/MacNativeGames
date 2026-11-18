#pragma once
#import <Foundation/Foundation.h>
#include "picture_profile.h"

#ifdef GAME_BOTW
#define PP_THIS_GAME PP_GAME_BOTW
#else
#define PP_THIS_GAME PP_GAME_TOTK
#endif

/* The picture choices as the app stores them (NSUserDefaults; the Settings pane writes the same two keys), for main.m
   (the environment at game start), the start screen and the Advanced screen. Tears of the Kingdom and Breath of the
   Wild only (not GAME_NS2). Keys: TOTK_PICTURE_PROFILE and TOTK_METALFX (the pane's rows), TOTK_CUSTOM_MODE / _SCENE /
   _TAA / _FPS / _SWITCH_SIZE / _ASPECT (Custom, written by the app only), TOTK_PICTURE_MIGRATED. */

/* What the game will use: an unknown profile is Performance, Custom without a saved combination is Performance's
   values, an unknown MetalFX value is Desligado. */
pp_picture totk_current_picture(pp_profile *profile, pp_metalfx *mfx);
BOOL totk_has_custom_picture(void);                       /* a Custom combination is saved (TOTK_CUSTOM_MODE) */
void totk_choose_profile(pp_profile p);                   /* TOTK_PICTURE_PROFILE */
void totk_save_custom_picture(const pp_picture *p);       /* TOTK_CUSTOM_* and the profile "custom" */
void totk_choose_metalfx(pp_metalfx m);                   /* TOTK_METALFX; the profile stays */
/* Once (TOTK_PICTURE_MIGRATED): an app that stored any old picture row becomes Custom with exactly what it ran
   before (picture_profile.c pp_migrate); a new install keeps the default, Performance. */
void totk_migrate_picture_rows(void);
