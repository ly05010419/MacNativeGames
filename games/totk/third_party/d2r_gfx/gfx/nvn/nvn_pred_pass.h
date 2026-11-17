// nvn_pred_pass.h -- a render pass's depth / stencil attachments, decided by one rule for the replay and for pipeline prediction.
//
// The replay attaches a pass's depth and stencil planes only when the pass uses them (pass_ds_need, ds_scan): from the draw
// that opens the pass to the op that ends it, a draw that tests or writes depth, or a fragment function that exports depth,
// needs the depth plane; a draw that tests stencil, the stencil plane; a clear inside the pass, a wait, or anything not
// provable, both. Pipeline prediction (pso_predict_rest) used to guess per draw instead: besides the pass's full planes it
// compiled a variant without depth for every draw that did not test depth itself. In a pass where another draw tests depth
// the replay attaches it, so that guess was never drawn: Ocarina of Time 1.0.1 on the Mac with the app's lists in the
// archive only (TOTK_WARM_SHARED=0) compiled 8 such pipelines 64 times in one 4.5-minute run, none of them ever drawn
// (TOTK_PSO_TRACE, 2026-11-16), and Breath of the Wild 1.6.0 1,062 guesses 12,417 times in 6 minutes (2026-10-28).
// Prediction now follows the passes the replay will make (pred_pass below) and gives each draw's key the planes of its pass:
// a pass's keys are held until the pass is decided, then compiled.
// 一个渲染通道挂不挂深度 / 模板，回放和管线预测用同一条规则：预测不再按单次绘制猜“去掉深度”的变体，
// 而是按回放会开的通道走，通道定下深度 / 模板后，再把这个通道的管线丢给后台编。
#ifndef NVN_PRED_PASS_H
#define NVN_PRED_PASS_H
#include <stdint.h>
#include "nvn_obj.h"

enum { DS_NEED_DEPTH = 1, DS_NEED_STENCIL = 2, DS_NEED_BOTH = 3 };

// An op at which the replay ends the pass: depth and stencil are not used by it any more (ds_scan's DS_FREE).
static inline int ds_op_ends_pass(uint32_t op) {
    switch (op) {
        case OP_SET_RT: case OP_CLEAR_BUF: case OP_COPY_B2B: case OP_UPDATE_UB: case OP_COPY_B2T: case OP_COPY_T2B:
        case OP_COPY_T2T: case OP_CLEAR_TEX: case OP_DISPATCH: return 1;
        default: return 0;
    }
}

// The planes one draw needs: its depth test or write, or a depth-exporting fragment function; its stencil test.
static inline int ds_draw_need(const ds_st *ds, int writes_depth) {
    return (ds->dt || ds->dw || writes_depth ? DS_NEED_DEPTH : 0) | (ds->st ? DS_NEED_STENCIL : 0);
}

// The planes a pass of these full formats attaches for a need (pass_ds_need's result, 3 when not provable).
static inline void ds_need_planes(int need, uint32_t dfull, uint32_t sfull, uint32_t *d, uint32_t *s) {
    *d = need & DS_NEED_DEPTH ? dfull : 0; *s = need & DS_NEED_STENCIL ? sfull : 0;
}

/* The replay's passes as prediction scans the commands (the replay's own steps in brackets):
   - a draw with no pass open opens one [draw: rp_begin_ex(mode of pass_ds_need)]: undecided until the op that ends it,
     its need being the attached clears' planes and every draw's (ds_scan); on a GPU without the look-ahead it attaches both
     [need = 3 when !tbdr_on()];
   - an op that ends passes (ds_op_ends_pass) closes it; so does a SetRenderTargets, unless the pass is open and the targets
     are the same: then the pass goes on with the planes it has [OP_SET_RT: same-target continuation], and a later draw
     that needs a plane it lacks ends it and opens a new pass [draw_inner: g_rt_merged];
   - before a pass's first draw a full clear is attached to it [R.clear_d / R.clear_s, R.clear_c]; any other clear is drawn
     as a quad in a pass with both planes [clear_quad, clear_ds_quad: rp_begin()]. In an open pass a clear ends a continued
     pass first [OP_CLEAR_*: rp_end when g_rt_merged], makes an undecided pass keep both planes [ds_scan: DS_USED], and a
     depth / stencil clear of a plane a decided pass lacks opens a pass with both [clear_ds_quad];
   - an event wait makes an undecided pass keep both planes [ds_scan: DS_USED].
   A draw's key gets its pass's planes: pred_pass_draw returns PRED_KEY_NOW when the pass is decided (planes p->need), else
   PRED_KEY_HELD (the caller holds the key). Whenever an undecided pass is decided, the step sets *held to its need: the
   caller gives the held keys those planes and compiles them. */
enum { PRED_KEY_NOW = 1, PRED_KEY_HELD = 2 };
typedef struct { int open, decided, cont, pend, need; } pred_pass;   // pend: clears attached to the next pass's start (need)

static inline void pred_pass_decide(pred_pass *p, int *held) { if (p->open && !p->decided) { *held = p->need; p->decided = 1; } }
static inline void pred_pass_close(pred_pass *p, int *held) {
    pred_pass_decide(p, held); p->open = p->decided = p->cont = p->pend = p->need = 0;
}
// The pass the replay is drawing when the scan starts: open with the planes it has; cont: it is a continued pass.
static inline void pred_pass_current(pred_pass *p, int planes, int cont) {
    p->open = 1; p->decided = 1; p->cont = cont; p->pend = 0; p->need = planes;
}
// An op of ds_op_ends_pass other than SetRenderTargets.
static inline void pred_pass_end(pred_pass *p, int *held) { pred_pass_close(p, held); }
// SetRenderTargets; same: the targets the pass draws into, or the ones its pending clears are for (rt_same); cont: the
// replay continues passes (tbdr_on() or TOTK_RT_CONTINUE).
static inline void pred_pass_set_rt(pred_pass *p, int same, int cont, int *held) {
    if (p->open && same && cont) { pred_pass_decide(p, held); p->cont = 1; return; }
    if (!p->open && p->pend && same && cont) return;   // OOT_KEEP_CLEARS: the clears wait for the next draw of the same targets
    pred_pass_close(p, held);
}
// A clear; attached: a full clear the replay attaches to a pass's start (mask, scissor); planes: the planes it clears.
static inline void pred_pass_clear(pred_pass *p, int attached, int planes, int *held) {
    if (p->open && p->cont) pred_pass_close(p, held);   // a clear ends a continued pass and goes to the next pass's start
    if (!p->open) {
        if (attached) { p->need |= planes; p->pend = 1; }
        else { p->open = p->decided = 1; p->cont = p->pend = 0; p->need = DS_NEED_BOTH; }   // clear_quad / clear_ds_quad
        return;
    }
    if (!p->decided) p->need = DS_NEED_BOTH;   // ds_scan: a clear inside the pass is not provable
    else if (planes & ~p->need) p->need = DS_NEED_BOTH;   // clear_ds_quad ends the pass, opens one with both planes
}
static inline void pred_pass_wait(pred_pass *p) { if (p->open && !p->decided) p->need = DS_NEED_BOTH; }
/* A draw needing these planes (ds_draw_need); tbdr: the replay looks ahead (pass_ds_need), else every pass it opens has both
   planes. A draw whose program prediction cannot resolve counts by its depth-stencil state alone: whether its fragment
   function exports depth is not known without loading it, and such functions are rare (if it does, the replay attaches
   depth and that pass's keys made without it are not drawn: a pipeline compiled for nothing, as every guess was before). */
static inline int pred_pass_draw(pred_pass *p, int planes, int tbdr, int *held) {
    if (p->open && p->decided && p->cont && (planes & ~p->need)) pred_pass_close(p, held);   // draw_inner: g_rt_merged
    if (!p->open) {
        p->open = 1; p->cont = p->pend = 0; p->need |= planes; p->decided = !tbdr;
        if (!tbdr) p->need = DS_NEED_BOTH;
        return p->decided ? PRED_KEY_NOW : PRED_KEY_HELD;
    }
    if (p->decided) return PRED_KEY_NOW;
    p->need |= planes; return PRED_KEY_HELD;
}

#endif
