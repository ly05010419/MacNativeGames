/* zeldaVR, Ocarina of Time, first person: Link's own body as the player sees it. Only the draws into the scene's targets
   change, so his shadow stays whole. Link is told from the file's data, not from a guess: his skinning palette (vertex
   uniform block 3) is exactly (smooth + rigid) x 48 bytes of his model, and the draw's vertex buffer holds exactly as many
   vertices as one of that model's shapes. Each shape's vertices are marked by their bone weights (oot_bfres.h): at least
   half on the clavicle or the arm below it. A draw keeps the triangles whose three vertices are marked, in any level of
   detail, so sleeves merged into another material's shape come along and the rest of that shape goes.
   The models: extracted/oot/vr-rig/ (games/oot/scripts/make_vr_rig.py, from the player's own RomFS). Without them the body
   is drawn whole. Included by nvn_mtl.m after body_tris_draw. */
#include "../../../../zeldaVR/runtime/oot_bfres.h"

extern int runtime_oot_vr_fp_active(void) __attribute__((weak_import));
extern void runtime_oot_vr_set_child(int child) __attribute__((weak_import));
int vr_oot_body_mode(void);   /* totk_vr.m, option body: 0 the arms, 1 nothing, 2 the whole body */

enum { OOT_RIG_SHAPES = 64, OOT_BODY_TRIS = 256 };
typedef struct { unsigned verts; uint8_t *arm; int any; } oot_rig_shape;   /* arm[v]: bit 0 an arm's vertex, bit 1 a leg's (Waist and below); any: the bits of its vertices */
enum { OOT_MARK_ARMS = 1, OOT_MARK_LEGS = 2 };
typedef struct {
    uint8_t *file; oot_bfres m; uint32_t *arms, *legs; unsigned palette_bytes, nshapes;
    oot_rig_shape shapes[OOT_RIG_SHAPES];
} oot_rig;
static oot_rig g_oot_rigs[2];
static _Atomic int g_oot_rig_state;   /* 0 not asked, 1 loading, 2 ready, 3 not available */

static const char *oot_rig_load_one(oot_rig *r, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "no file (an App packaged before the VR body: update it)";
    fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (64L << 20)) { fclose(f); return "unexpected size"; }
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(data); return "cannot read it"; }
    fclose(f);
    const char *e = oot_bfres_open(&r->m, data, (size_t)size);
    if (e) { free(data); return e; }
    r->file = data;
    r->palette_bytes = oot_bfres_matrix_count(&r->m) * 48;
    r->arms = calloc(OOT_BFRES_BONESET_WORDS(r->m.bone_count), 4);
    if (!r->arms) return "out of memory";
    static const char *const roots[4] = { "Clavicle_L", "Clavicle_R", "Arm_1_L", "Arm_1_R" };
    for (int i = 0; i < 4; i++) {
        const int b = oot_bfres_find_bone(&r->m, roots[i]);
        if (b < 0) return "no arm bones";
        e = i < 2 ? oot_bfres_boneset_add(&r->m, r->arms, (unsigned)b) : oot_bfres_boneset_add_subtree(&r->m, r->arms, (unsigned)b);
        if (e) return e;
    }
    {   /* the legs (oot_vr_legs.inc): Waist and everything under it; a model without a Waist has none */
        const int waist = oot_bfres_find_bone(&r->m, "Waist");
        r->legs = waist >= 0 ? calloc(OOT_BFRES_BONESET_WORDS(r->m.bone_count), 4) : NULL;
        if (r->legs && (e = oot_bfres_boneset_add_subtree(&r->m, r->legs, (unsigned)waist))) return e;
    }
    r->nshapes = r->m.shape_count < OOT_RIG_SHAPES ? r->m.shape_count : OOT_RIG_SHAPES;
    for (unsigned s = 0; s < r->nshapes; s++) {
        oot_bfres_shape shape; e = oot_bfres_shape_at(&r->m, s, &shape);
        if (e) return e;
        oot_rig_shape *o = &r->shapes[s]; o->verts = shape.vertex_count; o->arm = calloc(shape.vertex_count ? shape.vertex_count : 1, 1);
        if (!o->arm) return "out of memory";
        for (unsigned v = 0; v < shape.vertex_count; v++) {
            int in = 0, leg = 0; e = oot_bfres_vertex_in_set(&shape, v, r->arms, &in);
            if (!e && r->legs) e = oot_bfres_vertex_in_set(&shape, v, r->legs, &leg);
            if (e) return e;
            o->arm[v] = (uint8_t)((in ? OOT_MARK_ARMS : 0) | (leg ? OOT_MARK_LEGS : 0)); o->any |= o->arm[v];
        }
    }
    return NULL;
}
static void oot_items_load(void);   /* oot_vr_props.inc */
static void oot_wear_load(void);    /* oot_vr_wear.inc */
static void oot_legs_load(void);    /* oot_vr_legs.inc */
static void oot_rig_load(void) {
    static const char *const paths[2] = { "extracted/oot/vr-rig/Player_Child.bfres", "extracted/oot/vr-rig/Player_Adult.bfres" };
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        const char *e = oot_rig_load_one(&g_oot_rigs[i], paths[i]);
        if (e) { MLOG("[vr] Link's body: %s: %s; he is drawn whole", paths[i], e); ok = 0; break; }
        unsigned arms = 0; for (unsigned s = 0; s < g_oot_rigs[i].nshapes; s++) arms += g_oot_rigs[i].shapes[s].any != 0;
        MLOG("[vr] Link's body: %s, %u bones, palette %u bytes, %u shapes, %u with arm triangles", i ? "adult" : "child",
             g_oot_rigs[i].m.bone_count, g_oot_rigs[i].palette_bytes, g_oot_rigs[i].nshapes, arms);
    }
    if (ok) { oot_items_load(); oot_wear_load(); oot_legs_load(); }
    atomic_store(&g_oot_rig_state, ok ? 2 : 3);
}
static const oot_rig *oot_rig_for(uint64_t palette_bytes) {
    int state = atomic_load_explicit(&g_oot_rig_state, memory_order_acquire);
    if (state == 0) {
        int none = 0;
        if (atomic_compare_exchange_strong(&g_oot_rig_state, &none, 1)) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ oot_rig_load(); });
        return NULL;
    }
    if (state != 2) return NULL;
    for (int i = 0; i < 2; i++) if (g_oot_rigs[i].palette_bytes == palette_bytes) return &g_oot_rigs[i];
    return NULL;
}

typedef struct { uint64_t ibuf, vb; int32_t count, base; body_tris_t bt; int used, mask; } oot_body_entry;
static oot_body_entry g_oot_body[OOT_BODY_TRIS];
static uint64_t g_oot_body_drawn, g_oot_body_hidden;

static body_tris_t *oot_body_tris(const p_draw_idx *di, const oot_rig_shape *shape, int mask) {
    const uint64_t vb = S.vbuf[0].address;
    oot_body_entry *free_entry = NULL;
    for (int i = 0; i < OOT_BODY_TRIS; i++) {
        oot_body_entry *x = &g_oot_body[i];
        if (!x->used) { free_entry = x; break; }
        if (x->ibuf == di->ibuf && x->vb == vb && x->count == (int32_t)di->count && x->base == di->base_vertex && x->mask == mask) return x->bt.failed ? NULL : &x->bt;
    }
    if (!free_entry) return NULL;
    oot_body_entry *x = free_entry;
    x->used = 1; x->ibuf = di->ibuf; x->vb = vb; x->count = (int32_t)di->count; x->base = di->base_vertex; x->mask = mask; x->bt.failed = 1;
    if (di->prim != 4 || di->itype < 1 || di->itype > 2 || di->count < 3) return NULL;
    const int isz = di->itype == 1 ? 2 : 4; void *mb; uint64_t mo;
    if (!mtl_addr(di->ibuf, (uint64_t)di->count * isz, &mb, &mo)) return NULL;
    const uint8_t *ix = (const uint8_t *)[(__bridge id<MTLBuffer>)mb contents] + mo;
    id<MTLBuffer> out = [g_dev newBufferWithLength:(NSUInteger)di->count * isz options:MTLResourceStorageModeShared];
    if (!out) return NULL;
    res_add(out);
    uint8_t *to = out.contents; int n = 0;
    for (int t = 0; t + 2 < (int)di->count; t += 3) {
        int keep = 1;
        for (int j = 0; j < 3 && keep; j++) {
            const int64_t v = (int64_t)(isz == 2 ? ((const uint16_t *)(const void *)ix)[t + j] : ((const uint32_t *)(const void *)ix)[t + j]) + di->base_vertex;
            keep = v >= 0 && v < (int64_t)shape->verts && (shape->arm[v] & mask);
        }
        if (keep) { memcpy(to + (size_t)n * isz, ix + (size_t)t * isz, 3 * (size_t)isz); n += 3; }
    }
    x->bt.buf = out; x->bt.n = n; x->bt.itype = di->itype; x->bt.count = (int32_t)di->count; x->bt.ibuf = di->ibuf; x->bt.vb0 = vb; x->bt.failed = 0;
    return &x->bt;
}
/* -1: this draw is not drawn; 0: as the game draws it; 1: g_body_tris_cur has its arm triangles. */
static int oot_prop_draw(__unsafe_unretained NvShader *vs);   /* oot_vr_props.inc */
static int oot_wear_draw(const p_draw_idx *di, __unsafe_unretained NvShader *vs);   /* oot_vr_wear.inc */
static int oot_legs_on(const oot_rig *r);                      /* oot_vr_legs.inc */
static int g_oot_palette_reg = -1;   /* the vertex uniform block holding this draw's palette (pass dump arms03: 3 or 4, by shader) */
/* Diagnostic, off by default: "flag vr_body_trace <frames>" logs each draw holding Link's palette as it enters draw_inner, and
   what oot_body_draw decided and why. It tells a draw the game never made from one first person dropped. */
static const char *g_oot_body_why = "";
static float oot_block_opacity(const p_bind_buf *u);   /* oot_vr_whole.inc */
static _Atomic long g_oot_body_trace_until = -1;
#define OOT_BODY_RET(value, why) do { g_oot_body_why = (why); return (value); } while (0)
void oot_body_trace_next(long n) {
    const long until = n > 0 ? (long)atomic_load(&g_cur_frame) + n : -1;
    atomic_store(&g_oot_body_trace_until, until);
    MLOG("[vr-body] tracing Link's draws until frame %ld", until);
}
static int oot_body_trace_on(void) {
    const long until = atomic_load_explicit(&g_oot_body_trace_until, memory_order_relaxed);
    return until >= 0 && (long)atomic_load_explicit(&g_cur_frame, memory_order_relaxed) <= until;
}
static int oot_body_tracing(void) {
    const long until = atomic_load_explicit(&g_oot_body_trace_until, memory_order_relaxed);
    if (until < 0 || (long)atomic_load_explicit(&g_cur_frame, memory_order_relaxed) > until) return 0;
    for (int reg = 3; reg <= 4; reg++) if (S.ubo[0][reg].size == 13440 || S.ubo[0][reg].size == 13488) return 1;
    return 0;
}
static void oot_body_trace_entry(__unsafe_unretained NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    if (!oot_body_tracing()) return;
    NSString *vs = pr ? pr->key[0] : nil, *fs = pr ? pr->key[1] : nil;
    MLOG("[vr-body] F%llu enter count=%d indexed=%d pass=%dx%d rt=%d mask=%#x missing=%#x vs=%.16s fs=%.16s", (unsigned long long)atomic_load(&g_cur_frame),
         di ? (int)di->count : dw ? (int)dw->count : -1, di != NULL, (int)g_pass.tw, (int)g_pass.th, (int)R.rt.n, (unsigned)S.chmask.mask, pr ? (unsigned)pr->missing : 0,
         vs ? vs.UTF8String : "-", fs ? fs.UTF8String : "-");
}
static void oot_body_trace_result(const p_draw *dw, const p_draw_idx *di, int body) {
    if (!oot_body_tracing()) return;
    MLOG("[vr-body] F%llu decided count=%d body=%d why=%s palette=%d opacity=%.3f/%.3f dw=%d df=%d", (unsigned long long)atomic_load(&g_cur_frame),
         di ? (int)di->count : dw ? (int)dw->count : -1, body, g_oot_body_why, g_oot_palette_reg,
         (double)oot_block_opacity(&S.ubo[0][2]), (double)oot_block_opacity(&S.ubo[1][2]), (int)S.ds.dw, (int)S.ds.df);
}
static void oot_arrow_fx_note(__unsafe_unretained NvShader *vs);   /* oot_vr_bow.inc: a magic arrow's effects follow the VR arrow */
static int oot_body_draw(const p_draw_idx *di, __unsafe_unretained NvShader *vs) {
    g_oot_palette_reg = -1;
    oot_arrow_fx_note(vs);
    if (!vs) OOT_BODY_RET(0, "no vertex shader");
    uint64_t bytes = 0;
    for (int i = 0; i < vs->rf.nb && !bytes; i++) {
        if (vs->rf.b[i].kind != NVN_RB_CBUF || vs->rf.b[i].reg < 0 || vs->rf.b[i].reg >= 16) continue;
        const uint64_t size = S.ubo[0][vs->rf.b[i].reg].size;
        if (size == 13440 || size == 13488) { bytes = size; g_oot_palette_reg = vs->rf.b[i].reg; }
    }
    if (!bytes) {
        const int worn = oot_wear_draw(di, vs);   /* a tunic or gauntlets on the arms, the bracelet, or something worn left out */
        if (worn) OOT_BODY_RET(worn, "what Link wears (oot_vr_wear.inc)");
        const int prop = oot_prop_draw(vs); OOT_BODY_RET(prop, "not a palette in this shader's blocks: prop rule"); }
    if (runtime_oot_vr_set_child) runtime_oot_vr_set_child(bytes == 13440);
    if (!runtime_oot_vr_fp_active || !runtime_oot_vr_fp_active() || !di || R.rt.n < 1) OOT_BODY_RET(0, "not first person, not indexed, or no colour target");
    if (g_pass.tw < 640 || g_pass.th < 360 || g_pass.tw == g_pass.th) OOT_BODY_RET(0, "shadow or small target");   /* his shadow and reflections stay whole */
    const int mode = vr_oot_body_mode();
    if (mode == 2) OOT_BODY_RET(0, "body option: whole");
    const oot_rig *r = oot_rig_for(bytes);
    if (!r) OOT_BODY_RET(0, "no model for this palette");
    const uint32_t stride = S.stream[0].stride;
    if (!stride || !S.vbuf[0].size) OOT_BODY_RET(0, "no vertex stream");
    const unsigned verts = (unsigned)(S.vbuf[0].size / stride);
    const oot_rig_shape *shape = NULL;
    for (unsigned s = 0; s < r->nshapes && !shape; s++) if (r->shapes[s].verts == verts) shape = &r->shapes[s];
    if (!shape) OOT_BODY_RET(0, "no shape with this vertex count");   /* another model with a palette of this size */
    const int mask = OOT_MARK_ARMS | (oot_legs_on(r) ? OOT_MARK_LEGS : 0);   /* the legs too while they step (oot_vr_legs.inc) */
    if (mode == 1 || !(shape->any & mask)) { g_oot_body_hidden++; OOT_BODY_RET(-1, mode == 1 ? "body option: hidden" : "shape has no arm triangles"); }
    body_tris_t *bt = oot_body_tris(di, shape, mask);
    if (!bt) { g_oot_body_hidden++; OOT_BODY_RET(-1, "no triangle list (table full or failed)"); }
    if (!bt->n) { g_oot_body_hidden++; OOT_BODY_RET(-1, "no arm triangles in this draw"); }
    g_body_tris_cur = bt; g_oot_body_drawn++;
    if (g_oot_body_drawn == 1) MLOG("[vr] Link's body: first person shows his arms (%d of %d indices of this draw)", bt->n, di->count);
    OOT_BODY_RET(1, "arm triangles");
}
#include "oot_vr_arms.inc"
#include "oot_vr_song.inc"
