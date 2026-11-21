#pragma once

/* Picture profiles on iPhone / iPad (PR #2287, user 2026-11-17; iPad default Performance, user 2026-11-18):
   Quality, Performance and Economy are fixed combinations of the Advanced screen's controls; Custom is the
   player's own. MetalFX (the upscale) is a setting of its own, on top of any profile. Kept free of UIKit so
   tests/test_picture_profile.c checks it on a Mac. The environment it builds is what the runtime reads at game start. */

typedef enum { PP_QUALITY, PP_PERFORMANCE, PP_ECONOMY, PP_CUSTOM } pp_profile;
typedef enum { PP_HANDHELD, PP_DOCKED } pp_mode;
typedef enum { PP_MFX_OFF, PP_MFX_SCENE, PP_MFX_SCREEN } pp_metalfx;
/* PP_GAME_TOTK_OLD: Tears of the Kingdom 1.0 / 1.2.1, whose handheld scene without a size is the game's own 1280x720
   (game_patches.c render_res), where 1.4.3's and Breath of the Wild's is 960x540 (after PR #2287). */
/* PP_GAME_ACNH: Animal Crossing (games/acnh/ios builds with these files): no scene size, edge smoothing or 16:9 bars of
   its own; the mode, MetalFX to the screen and 30 / 60 (ACNH_FPS: its 60 FPS text, exported on the Mac). */
/* PP_GAME_ODYSSEY: Super Mario Odyssey, as Animal Crossing without a frame-rate choice (its own 60; its render size is a
   code patch the phone cannot make at run time). */
/* PP_GAME_MK8D: Mario Kart 8 Deluxe (games/mk8d/ios, the nxnative line's host), Super Mario Odyssey's picture: the mode and
   MetalFX to the screen, no frame-rate choice (its own 60); they reach its host as DM_OPMODE and DM_PRESENT_UPSCALE
   (games/mk8d/ios/Sources/ios_front.m). The same value, so the other games' code is as it was. */
/* PP_GAME_PLA: Pokémon Legends: Arceus (games/pla/ios), Super Mario Odyssey's picture: the mode (TOTK_OPERATION_MODE) and
   MetalFX to the screen, no frame-rate choice (its own 30 step). The same value, so the other games' code is as it was. */
/* PP_GAME_MK8DX: Mario Kart 8 Deluxe on the Zelda line (games/mk8dx/ios, Super Mario Odyssey's runtime), Super Mario
   Odyssey's picture: the mode (TOTK_OPERATION_MODE) and MetalFX to the screen, no frame-rate choice (its own 60). The
   same value, so the other games' code is as it was. */
typedef enum { PP_GAME_TOTK, PP_GAME_BOTW, PP_GAME_TOTK_OLD, PP_GAME_ACNH, PP_GAME_ODYSSEY, PP_GAME_MK8D = PP_GAME_ODYSSEY,
               PP_GAME_MK8DX = PP_GAME_ODYSSEY, PP_GAME_PLA = PP_GAME_ODYSSEY } pp_game;

typedef struct {
    pp_mode mode;
    unsigned scene_w;    /* 0: automatic (the game's own handheld scene, 1600x900 docked); else one of the listed widths */
    int taa;             /* edge smoothing (MetalFX temporal anti-aliasing) */
    int fps;             /* 30 or 60 (60: no fixed cap) */
    int aspect_169;      /* Tears of the Kingdom only: 16:9 with bars instead of filling the screen */
    int present_60;      /* Custom only, from the old custom layer's 60 fps cap: the display paces 60 (DM_PRESENT_HZ=60),
                            as it did before the profiles, instead of the uncapped path; any frame-rate change clears it
                            (after PR #2287) */
} pp_picture;

#define PP_ENV_MAX 12
typedef struct { const char *name, *value; } pp_var;   /* value NULL: leave the variable as it is */
typedef struct { pp_var v[PP_ENV_MAX]; int n; } pp_env;

const char *pp_profile_key(pp_profile p);             /* "quality" | "performance" | "economy" | "custom" */
int pp_profile_parse(const char *s, pp_profile *out); /* 0 for anything else (out untouched) */
const char *pp_metalfx_key(pp_metalfx m);             /* "off" | "scene" | "screen" */
int pp_metalfx_parse(const char *s, pp_metalfx *out);
int pp_scene_parse(const char *s, unsigned *w);       /* "" → 0 (automatic); the six listed sizes → width; else 0 returned */
const char *pp_scene_key(unsigned w);                 /* the stored form: "" for 0 (or a width not listed), else "<w>x<h>" */

int pp_profile_picture(pp_profile p, pp_picture *out);  /* the fixed values; 0 for PP_CUSTOM (out untouched) */
/* The widest scene the game takes in the picture's mode: docked 1920; handheld 1280 (the canvas) for Breath of the Wild,
   1600 for Tears of the Kingdom (its scene is scaled by the canvas; above 1600x900 it needs heaps the phone has not). */
unsigned pp_scene_limit(const pp_picture *p, pp_game g);
unsigned pp_scene_effective(const pp_picture *p, pp_game g);   /* automatic resolved, held at pp_scene_limit */
int pp_mfx_scene_acts(const pp_picture *p, pp_game g);         /* 0 unless the scene is narrower than the canvas */
void pp_env_build(const pp_picture *p, pp_metalfx m, pp_game g, pp_env *out);

/* The old Settings rows (before the profiles) as they were saved, for the one-time migration to Custom. */
typedef struct {
    int docked;                /* TOTK_DOCKED */
    const char *docked_scene;  /* TOTK_DOCKED_SCENE (TotK) or TOTK_BOTW_SCENE */
    const char *phone_fps;     /* TOTK_PHONE_FPS */
    int custom_picture;        /* TOTK_CUSTOM_PICTURE: Tears of the Kingdom's custom layer */
    const char *picture_mode;  /* TOTK_IOS_PICTURE_MODE: "720" | "1080" | "mfx" */
    const char *render_res;    /* TOTK_RENDER_RES (the custom layer's scene) */
    const char *fps_cap;       /* TOTK_FPS_CAP */
    int mfx_upscale;           /* TOTK_MFX_UPSCALE */
    int taa;                   /* TOTK_TAA */
    const char *aspect;        /* TOTK_ASPECT: "0" / "16:9" is 16:9 */
    int totk_old;              /* Tears of the Kingdom 1.0 / 1.2.1: its handheld scene without a size is 1280x720 */
} pp_old;
/* Exactly the combination that ran before (user 2026-11-17); MetalFX goes to its own setting. */
void pp_migrate(const pp_old *old, pp_game g, pp_picture *custom, pp_metalfx *mfx);
