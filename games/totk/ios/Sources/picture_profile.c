#include "picture_profile.h"
#include <string.h>

static const char *const PROFILE_KEYS[] = {"quality", "performance", "economy", "custom"};
static const char *const METALFX_KEYS[] = {"off", "scene", "screen"};

/* The scene sizes: what game_patches_botw.c phone_scene_custom and TotK's render_res take on the phone. */
static const struct { const char *size; unsigned width; } SCENES[] = {
    {"1920x1080", 1920}, {"1600x900", 1600}, {"1280x720", 1280}, {"960x540", 960}, {"800x450", 800}, {"640x360", 640}};

/* The profiles (user 2026-11-17): Quality the sharpest, docked; Performance the default, handheld at the game's own
   phone scene; Economy the coolest, at the Switch's screen size. */
static const pp_picture PROFILES[] = {
    [PP_QUALITY]     = {PP_DOCKED,   1600, 1, 30, 0, 0, 0},   /* 30: steady where 1600x900 + TAA cannot hold 60 (iPad A16: 33) */
    [PP_PERFORMANCE] = {PP_HANDHELD, 0,    0, 60, 0, 0, 0},
    [PP_ECONOMY]     = {PP_HANDHELD, 640,  0, 30, 1, 0, 0},
};

static int parse_key(const char *s, const char *const *keys, int n) {
    if (s) for (int i = 0; i < n; i++) if (!strcmp(s, keys[i])) return i;
    return -1;
}

const char *pp_profile_key(pp_profile p) { return PROFILE_KEYS[p]; }
int pp_profile_parse(const char *s, pp_profile *out) {
    int i = parse_key(s, PROFILE_KEYS, 4);
    if (i < 0) return 0;
    *out = (pp_profile)i;
    return 1;
}
const char *pp_metalfx_key(pp_metalfx m) { return METALFX_KEYS[m]; }
int pp_metalfx_parse(const char *s, pp_metalfx *out) {
    int i = parse_key(s, METALFX_KEYS, 3);
    if (i < 0) return 0;
    *out = (pp_metalfx)i;
    return 1;
}
int pp_scene_parse(const char *s, unsigned *w) {
    if (!s) return 0;
    if (!*s) { *w = 0; return 1; }
    for (unsigned i = 0; i < sizeof SCENES / sizeof *SCENES; i++) if (!strcmp(s, SCENES[i].size)) { *w = SCENES[i].width; return 1; }
    return 0;
}

const char *pp_scene_key(unsigned w) {
    for (unsigned i = 0; i < sizeof SCENES / sizeof *SCENES; i++) if (SCENES[i].width == w) return SCENES[i].size;
    return "";
}

int pp_profile_picture(pp_profile p, pp_picture *out) {
    if (p == PP_CUSTOM) return 0;
    *out = PROFILES[p];
    return 1;
}

/* The canvas: handheld 1280x720, docked 1920x1080. Breath of the Wild cannot make its heaps above it
   (game_patches_botw.c patch_render_1080); Tears of the Kingdom scales its scene by the canvas (game_patches_v143.c
   render_res_publish) and runs a handheld scene up to 1600x900, as its old custom layer offered (final review 2026-11-18). */
static unsigned canvas_w(const pp_picture *p) { return p->mode == PP_DOCKED ? 1920 : 1280; }
static int is_totk(pp_game g) { return g != PP_GAME_BOTW; }   /* every Tears of the Kingdom version */
unsigned pp_scene_limit(const pp_picture *p, pp_game g) {
    return p->mode == PP_DOCKED ? 1920 : is_totk(g) ? 1600 : 1280;
}
/* Automatic is the game's own scene: docked 1600x900; handheld 960x540, but 1280x720 on Tears of the Kingdom 1.0 / 1.2.1
   (it was passed as 960x540 there, below what those versions ran before; after PR #2287). */
unsigned pp_scene_effective(const pp_picture *p, pp_game g) {
    unsigned own = p->mode == PP_DOCKED ? 1600 : g == PP_GAME_TOTK_OLD ? 1280 : 960;
    unsigned w = p->scene_w ? p->scene_w : own, limit = pp_scene_limit(p, g);
    return w > limit ? limit : w;
}
int pp_mfx_scene_acts(const pp_picture *p, pp_game g) { return pp_scene_effective(p, g) < canvas_w(p); }

static void put(pp_env *e, const char *name, const char *value) { e->v[e->n].name = name; e->v[e->n].value = value; e->n++; }

/* What the runtime reads at game start (main.m apply_display_settings). The drawable: docked's 1080 rows, or the view's
   own pixels when MetalFX also enlarges the canvas to the screen (with the Switch size that view is the Switch-sized
   one, so the screen stage stops there). "60" is the uncapped path, or the display's 60 Hz pace for a Custom combination
   brought over from the old custom layer (present_60), "30" the phone's hold. Automatic is passed as the size it
   resolves to (pp_scene_effective). */
void pp_env_build(const pp_picture *p, pp_metalfx m, pp_game g, pp_env *out) {
    out->n = 0;
    unsigned w = pp_scene_effective(p, g);
    const char *scene = "960x540";
    for (unsigned i = 0; i < sizeof SCENES / sizeof *SCENES; i++) if (SCENES[i].width == w) scene = SCENES[i].size;
    put(out, "TOTK_OPERATION_MODE", p->mode == PP_DOCKED ? "docked" : "handheld");
    put(out, "TOTK_IOS_DRAWABLE_H", m == PP_MFX_SCREEN ? "0" : p->mode == PP_DOCKED ? "1080" : NULL);
    put(out, "TOTK_RENDER_RES", scene);
    put(out, "TOTK_MFX_UPSCALE", m == PP_MFX_OFF ? "0" : "1");
    put(out, "TOTK_MFX_OUTPUT", m == PP_MFX_SCREEN ? "1" : "0");
    put(out, "TOTK_TAA", p->taa ? "1" : "0");
    put(out, "TOTK_PHONE_FPS", p->fps == 30 ? "30" : NULL);
    put(out, "DM_PRESENT_HZ", p->fps == 60 && p->present_60 ? "60" : NULL);
    put(out, "TOTK_SWITCH_SIZE", p->switch_size ? "1" : "0");
    put(out, "TOTK_ASPECT", is_totk(g) && p->aspect_169 ? "0" : NULL);
}

static int is(const char *s, const char *v) { return s && !strcmp(s, v); }
void pp_migrate(const pp_old *old, pp_game g, pp_picture *custom, pp_metalfx *mfx) {
    pp_picture p = {PP_HANDHELD, 0, old->taa != 0, 60, 0, 0, 0};
    *mfx = old->mfx_upscale ? PP_MFX_SCENE : PP_MFX_OFF;
    if (is_totk(g) && old->custom_picture) {   /* the custom layer replaced the presets */
        p.mode = is(old->picture_mode, "1080") ? PP_DOCKED : PP_HANDHELD;
        if (is(old->picture_mode, "mfx")) *mfx = PP_MFX_SCREEN;
        unsigned w = 0;
        p.scene_w = pp_scene_parse(old->render_res, &w) && w ? w : 960;   /* its row's default was 960x540 */
        p.fps = is(old->fps_cap, "30") ? 30 : 60;
        p.present_60 = p.fps == 60;   /* its 60 cap paced the display (settings_map.c totk_frame_env_build, before PR #2287) */
    } else {
        p.mode = old->docked ? PP_DOCKED : PP_HANDHELD;
        p.scene_w = old->docked ? (is(old->docked_scene, "1920x1080") ? 1920 : 1600) : old->totk_old && is_totk(g) ? 1280 : 960;
        p.fps = is(old->phone_fps, "30") ? 30 : 60;
    }
    p.aspect_169 = is_totk(g) && (is(old->aspect, "0") || is(old->aspect, "16:9"));
    *custom = p;
}
