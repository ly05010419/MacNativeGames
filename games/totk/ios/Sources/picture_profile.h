#pragma once

/* Picture profiles on iPhone / iPad (docs/superpowers/specs/2026-11-17-ios-picture-profiles-design.md, user 2026-11-17):
   Qualidade, Desempenho and Economia are fixed combinations of the Advanced screen's controls; Personalizado is the
   player's own. MetalFX (the upscale) is a setting of its own, on top of any profile. Kept free of UIKit so
   tests/test_picture_profile.c checks it on a Mac. The environment it builds is what the runtime reads at game start. */

typedef enum { PP_QUALITY, PP_PERFORMANCE, PP_ECONOMY, PP_CUSTOM } pp_profile;
typedef enum { PP_HANDHELD, PP_DOCKED } pp_mode;
typedef enum { PP_MFX_OFF, PP_MFX_SCENE, PP_MFX_SCREEN } pp_metalfx;
typedef enum { PP_GAME_TOTK, PP_GAME_BOTW } pp_game;

typedef struct {
    pp_mode mode;
    unsigned scene_w;    /* 0: Automática (960x540 handheld, 1600x900 docked); else one of the listed widths */
    int taa;             /* edge smoothing (MetalFX temporal anti-aliasing) */
    int fps;             /* 30 or 60 (60: no fixed cap) */
    int switch_size;     /* the picture at the Switch's 6.2-inch screen (iPad only; the app checks) */
    int aspect_169;      /* Tears of the Kingdom only: 16:9 with bars instead of filling the screen */
} pp_picture;

#define PP_ENV_MAX 12
typedef struct { const char *name, *value; } pp_var;   /* value NULL: leave the variable as it is */
typedef struct { pp_var v[PP_ENV_MAX]; int n; } pp_env;

const char *pp_profile_key(pp_profile p);             /* "quality" | "performance" | "economy" | "custom" */
int pp_profile_parse(const char *s, pp_profile *out); /* 0 for anything else (out untouched) */
const char *pp_metalfx_key(pp_metalfx m);             /* "off" | "scene" | "screen" */
int pp_metalfx_parse(const char *s, pp_metalfx *out);
int pp_scene_parse(const char *s, unsigned *w);       /* "" → 0 (Automática); the six listed sizes → width; else 0 returned */

int pp_profile_picture(pp_profile p, pp_picture *out);  /* the fixed values; 0 for PP_CUSTOM (out untouched) */
unsigned pp_scene_effective(const pp_picture *p);     /* Automática resolved, held at the mode's canvas */
int pp_mfx_scene_acts(const pp_picture *p);           /* 0 when the scene is as wide as the canvas: nothing to upscale */
void pp_env_build(const pp_picture *p, pp_metalfx m, pp_game g, pp_env *out);

/* The old Settings rows (before the profiles) as they were saved, for the one-time migration to Personalizado. */
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
/* Exactly the combination that ran before (spec "Migração"); MetalFX goes to its own setting. */
void pp_migrate(const pp_old *old, pp_game g, pp_picture *custom, pp_metalfx *mfx);
