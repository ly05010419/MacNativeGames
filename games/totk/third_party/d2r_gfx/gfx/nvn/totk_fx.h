// Effect switches (Mac, TOTK; user 2026-10-30: "参考 ultracam 添加 … 着色器开关 都能单独开关"): the settings panel's effects
// page turns single picture effects off and on while the game runs. Game-specific and never needed to run the game;
// included by nvn_mtl.m after totk_ssao.h. Own implementation of UltraCam's idea (its shader toggles replace chosen shader
// programs; nothing of its code): an effect is the set of shaders whose draws (fragment shader key) or dispatches (compute
// shader key) are left out while it is off. The keys are TOTK 1.4.3's, found by leaving out each full-screen pass of a
// frame in turn and comparing screenshots (MacNativeGames-artifacts/claude/fx, "flag skipfs_<key>" / "skipcs_<key>"):
// only passes whose absence leaves a whole picture minus that effect are listed (a pass that feeds later ones, such as
// the deferred lighting tiles, would leave holes; one whose result the game reads back ends the game). Ambient occlusion
// is computed inside the shadow pre-pass with the shadows, so its switch is the AO patch's own (totk_ssao.h, c1 .w bit 0);
// the sun's shadows the same way (bit 1: that pass's shadow term written as lit).
typedef struct { const char *id, *zh, *en; const char *fs[6], *cs[4]; } fx_effect;
static const fx_effect k_fx[] = {
    {"ao", "环境光遮蔽", "Ambient occlusion", {0}, {0}},   // totk_ssao.h
    {"shadow", "阴影", "Shadows", {0}, {0}},   // every shadow-map comparison reads lit (nvn_mtl.m) and the shadow pre-pass's term too (totk_ssao.h: its far terrain shadow compares by hand)
    {"grass", "草", "Grass", {"33099f8c133950f5"}, {0}},   // the field's grass blades (2026-10-30 survey, v143-grass)
    /* The extra light the game puts on Link (61315f715cdf30ed) had a switch until 2026-10-31; the user: "角色补光 去掉 默认开
       不能关". It is always drawn; a "charlight" left in graphics-effects.txt is a word no effect has, so it is ignored. */
    /* The fog of the air: the scattering images at 1/8 and 1/16 of the screen (240x135 / 120x67 at 1080p) take additive
       draws, which a later pass spreads over the picture. Volumetric fog: the ray-marched light in the air (3231347c, a
       2300-line loop over the view ray). Fog: the two short layers added to the same images (c7df2e96 / f459822d, one
       texture read each); the same fragment shaders also write a 480x270 image in full with other vertex shaders, which
       stays, so these entries name the vertex shader too ("fragment/vertex"). Leaving out an additive draw leaves the
       images without that light (they are cleared each frame). */
    {"vfog", "体积雾", "Volumetric fog", {"3231347c1ef88f06"}, {0}},
    {"fog", "雾效", "Fog", {"c7df2e9641b1ee60/4c68cebf72d716f1", "f459822d1a4e71ab/da1c7d4fcff1d416"}, {0}},
};
enum { FX_N = sizeof k_fx / sizeof *k_fx, FX_AO = 0, FX_SHADOW = 1 };
static _Atomic uint32_t g_fx_off;   // bit i: k_fx[i] is off

/* The effect a shader belongs to: k_fx index + 1, or -1 for none; worked out at its first draw while any effect is off and
   kept in the shader (NvShader.fx, 0 = not looked up yet). Keys are compared on their 16 hex digits (a variant's key
   carries a suffix after them). An entry "fragment/vertex" also names the vertex shader: the fragment shader then gets
   the entry (with k_fx index + 1 + 1000) and each draw checks its vertex shader (fx_vs_match). */
static int fx_of(NvShader *o, int stage) {
    int v = o->fx;
    if (!v) {
        v = -1;
        for (int i = 0; i < FX_N && v < 0; i++) {
            const char *const *keys = stage == 5 ? k_fx[i].cs : k_fx[i].fs; int n = stage == 5 ? 4 : 6;
            for (int j = 0; j < n && keys[j]; j++) if (!strncmp(o->key, keys[j], 16)) { v = i + 1 + (keys[j][16] == '/' ? 1000 : 0); break; }
        }
        o->fx = v;
    }
    return v;
}
static int fx_vs_match(int i, __unsafe_unretained NvShader *fs, __unsafe_unretained NvShader *vs) {
    for (int j = 0; j < 6 && k_fx[i].fs[j]; j++)
        if (!strncmp(fs->key, k_fx[i].fs[j], 16) && k_fx[i].fs[j][16] == '/' && vs && !strncmp(vs->key, k_fx[i].fs[j] + 17, 16)) return 1;
    return 0;
}
/* Whether to leave out this draw (stage 1: its fragment shader, and vs its vertex shader for "fragment/vertex" entries)
   or dispatch (stage 5, its compute shader). */
static inline int fx_drop(NvShader *o, int stage, __unsafe_unretained NvShader *vs) {
    uint32_t off = atomic_load_explicit(&g_fx_off, memory_order_relaxed);
    if (!off || !o) return 0;
    int v = fx_of(o, stage), pair = v > 1000; if (pair) v -= 1000;
    if (!(v > 0 && (off >> (v - 1) & 1))) return 0;
    return !pair || fx_vs_match(v - 1, o, vs);
}

/* The settings panel (runtime/settings_panel.h): the effects in their order, their names, and the switch. AO without the
   AO patch (TOTK_SSAO=0) cannot be turned off: dm_gfx_fx_available says so. */
int dm_gfx_fx_count(void) { return FX_N; }
const char *dm_gfx_fx_id(int i) { return i >= 0 && i < FX_N ? k_fx[i].id : ""; }
const char *dm_gfx_fx_name(int i, int english) { return i >= 0 && i < FX_N ? (english ? k_fx[i].en : k_fx[i].zh) : ""; }
int dm_gfx_fx_available(int i) {
#ifdef GAME_BOTW
    (void)i; return 0;   // the keys are TOTK's
#else
    return i >= 0 && i < FX_N && ((i != FX_AO && i != FX_SHADOW) || ssao_on());
#endif
}
int dm_gfx_fx_on(int i) { return i >= 0 && i < FX_N && !(atomic_load(&g_fx_off) >> i & 1); }
void dm_gfx_fx_set(int i, int on) {
    if (!dm_gfx_fx_available(i)) return;
    if (on) atomic_fetch_and(&g_fx_off, ~(1u << i)); else atomic_fetch_or(&g_fx_off, 1u << i);
    if (i == FX_AO) ssao_set_off(!on);
    if (i == FX_SHADOW) { extern void dm_gfx_shadow_samplers_off(int off); ssao_set_shadow_off(!on); dm_gfx_shadow_samplers_off(!on); }
    MLOG("[fx] %s %s", k_fx[i].id, on ? "on" : "off");
}
