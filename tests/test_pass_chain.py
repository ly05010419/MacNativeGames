"""The pass chain (TOTK_PASS_CHAIN): a pass the game keeps leaving for passes that touch none of it is drawn once.

Ocarina of Time draws each character's shadow as "a 384x384 depth map of its own, then one box into a 1920x1080 buffer
that samples it": 25 of them in Castle Town were 25 passes that loaded and stored that buffer for one draw each
(2.06-2.39 ms of GPU time a frame, 2026-11-18). The deferral that came with DM_NVN_TBDR (off since it blackened Tears of
the Kingdom's menu) is used alone for this, with stricter conditions; under DM_NVN_TBDR it stays exactly as it was.
On for Ocarina of Time on the Mac, off everywhere else unless asked for.

The pre-screen runs here from the production source on recorded command streams; the conditions of the full look-ahead
and the places that keep a deferred draw out of a wrong pass are checked in the source."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


def function_body(src, signature):
    start = src.index(signature)
    return src[start:src.index('\n}\n', start) + 3]


def one_line(src, start):
    at = src.index(start)
    return src[at:src.index('\n', at) + 1]


PROBE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nvn_obj.h"
static struct { p_set_rt rt; } R;
static const uint8_t *g_exec_next;
static const uint8_t *g_rest_list;   /* the submission's next command list, once */
static const uint8_t *defer_cont(int *ci) { return (*ci)++ ? NULL : g_rest_list; }
/* PRODUCTION */
static uint8_t buf[2][1 << 16]; static size_t at[2];
static void op(int list, uint32_t code, const void *payload, size_t n) {
    op_hdr h = {code, (uint32_t)(8 + ((n + 7) & ~(size_t)7))}; memcpy(buf[list] + at[list], &h, 8);
    if (n) memcpy(buf[list] + at[list] + 8, payload, n);
    at[list] += h.size;
}
static void targets(int list, const tex_t *color, const tex_t *depth) {
    p_set_rt r; memset(&r, 0, sizeof r); r.n = color ? 1 : 0; r.color[0] = color; r.depth = depth; op(list, OP_SET_RT, &r, sizeof r);
}
static void draw_n(int list, int n) { p_draw d; memset(&d, 0, sizeof d); d.count = 36; for (int i = 0; i < n; i++) op(list, OP_DRAW, &d, sizeof d); }
static void reset(const tex_t *color, const tex_t *depth) {
    memset(buf, 0, sizeof buf); at[0] = at[1] = 0; g_rest_list = NULL; g_exec_next = buf[0];
    memset(&R, 0, sizeof R); R.rt.n = 1; R.rt.color[0] = color; R.rt.depth = depth;
}
static int pre(void) { op(0, OP_END, NULL, 0); return defer_chain_pre(); }
int main(void) {
    tex_t light, scene_depth, shadow1, shadow2, other, other_depth;
    memset(&light, 0, sizeof light); scene_depth = shadow1 = shadow2 = other = other_depth = light;
    light.serial = 23224; scene_depth.serial = 54725; shadow1.serial = 23751; shadow2.serial = 23752; other.serial = 9; other_depth.serial = 10;

    /* this draw is the light volume; then the next light's shadow map; then the light buffer again */
    reset(&light, &scene_depth); targets(0, NULL, &shadow1); draw_n(0, 8); targets(0, &light, &scene_depth); draw_n(0, 1);
    assert(pre() == 1);
    /* the game sets the same targets once more before leaving: still this pass */
    reset(&light, &scene_depth); targets(0, &light, &scene_depth); draw_n(0, 2); targets(0, NULL, &shadow1); draw_n(0, 8); targets(0, &light, &scene_depth);
    assert(pre() == 1);
    /* two other passes in between */
    reset(&light, &scene_depth); targets(0, NULL, &shadow1); draw_n(0, 8); targets(0, NULL, &shadow2); draw_n(0, 8); targets(0, &light, &scene_depth);
    assert(pre() == 1);
    /* the way back is in the submission's next command list */
    reset(&light, &scene_depth); targets(0, NULL, &shadow1); draw_n(0, 8); op(0, OP_END, NULL, 0);
    targets(1, &light, &scene_depth); draw_n(1, 1); op(1, OP_END, NULL, 0); g_rest_list = buf[1];
    assert(defer_chain_pre() == 1);

    /* never back */
    reset(&light, &scene_depth); targets(0, NULL, &shadow1); draw_n(0, 8);
    assert(pre() == 0);
    /* three other passes: out of reach */
    reset(&light, &scene_depth); targets(0, NULL, &shadow1); targets(0, NULL, &shadow2); targets(0, &other, &other_depth); targets(0, &light, &scene_depth);
    assert(pre() == 0);
    /* the pass in between draws into this pass's colour, or uses its depth */
    reset(&light, &scene_depth); targets(0, &light, &other_depth); draw_n(0, 1); targets(0, &light, &scene_depth);
    assert(pre() == 0);
    reset(&light, &scene_depth); targets(0, &other, &scene_depth); draw_n(0, 1); targets(0, &light, &scene_depth);
    assert(pre() == 0);
    /* back to the colour with another depth: not the same targets */
    reset(&light, &scene_depth); targets(0, NULL, &shadow1); draw_n(0, 8); targets(0, &light, &other_depth);
    assert(pre() == 0);
    /* more draws in this pass than a group holds: this draw and 63 more are its 64 */
    reset(&light, &scene_depth); draw_n(0, 64); targets(0, NULL, &shadow1); targets(0, &light, &scene_depth);
    assert(pre() == 0);
    reset(&light, &scene_depth); draw_n(0, 63); targets(0, NULL, &shadow1); targets(0, &light, &scene_depth);
    assert(pre() == 1);
    /* anything but draws, state and clears on the way */
    static const uint32_t stops[] = {OP_CALL, OP_WAIT_EVENT, OP_SIGNAL_EVENT, OP_FENCE_SYNC, OP_REPORT_COUNTER, OP_UPDATE_UB, OP_CLEAR_BUF, OP_COPY_B2B,
                                     OP_COPY_B2T, OP_COPY_T2B, OP_COPY_T2T, OP_CLEAR_TEX, OP_DISPATCH};
    for (size_t i = 0; i < sizeof stops / sizeof *stops; i++) {
        uint8_t zero[64] = {0};
        reset(&light, &scene_depth); targets(0, NULL, &shadow1); op(0, stops[i], zero, sizeof zero); targets(0, &light, &scene_depth);
        assert(pre() == 0);
    }

    /* the same texture, or memory in common: the game puts new textures on the same pool addresses within a frame */
    pool_t pool_a, pool_b; memset(&pool_a, 0, sizeof pool_a); memset(&pool_b, 0, sizeof pool_b);
    static uint8_t memory[1 << 16];
    tex_t x, y; memset(&x, 0, sizeof x); memset(&y, 0, sizeof y);
    x.serial = 1; x.mem = memory; x.L.size = 4096; x.pool = &pool_a;
    y.serial = 1; assert(defer_tex_alias(&x, &y) == 1);                                             /* one texture, a copy of the object */
    y.serial = 2; y.mem = memory; y.L.size = 256; y.pool = &pool_a; assert(defer_tex_alias(&x, &y) == 1);          /* another texture at the same address */
    y.mem = memory + 4095; assert(defer_tex_alias(&x, &y) == 1 && defer_tex_alias(&y, &x) == 1);     /* one byte in common */
    y.mem = memory + 4096; assert(defer_tex_alias(&x, &y) == 0 && defer_tex_alias(&y, &x) == 0);     /* next to each other */
    y.mem = NULL; assert(defer_tex_alias(&x, &y) == 0);                                              /* no pool memory: shares with nothing */
    y.mem = memory + 8192; y.L.size = 0; assert(defer_tex_alias(&x, &y) == 1);                       /* size not known: anything in its pool */
    y.pool = &pool_b; assert(defer_tex_alias(&x, &y) == 0);
    assert(defer_tex_alias(NULL, &x) == 0 && defer_tex_alias(&x, NULL) == 0);

    /* Q's segment: state, nothing that reads or writes memory, clears, waits, signals or reports */
    static const uint32_t state[] = {OP_BEGIN, OP_VIEWPORTS, OP_SCISSOR, OP_BIND_PROGRAM, OP_BIND_STATE, OP_BIND_VBUF, OP_BIND_UBO, OP_BIND_TEX, OP_BIND_SAMP, OP_BARRIER, OP_RENDER_ENABLE};
    static const uint32_t not_state[] = {OP_END, OP_JUMP, OP_CALL, OP_SIGNAL_EVENT, OP_REPORT_COUNTER, OP_COPY_B2B, OP_CLEAR_BUF, OP_UPDATE_UB, OP_SET_RT, OP_CLEAR_COLOR, OP_CLEAR_DS,
                                         OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_COPY_B2T, OP_COPY_T2B, OP_COPY_T2T, OP_RESET_COUNTER, OP_CLEAR_TEX, OP_FENCE_SYNC, OP_WAIT_EVENT};
    for (size_t i = 0; i < sizeof state / sizeof *state; i++) assert(defer_chain_state_op(state[i]) == 1);
    for (size_t i = 0; i < sizeof not_state / sizeof *not_state; i++) assert(defer_chain_state_op(not_state[i]) == 0);
    puts("pass chain pre-screen: ok");
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'clang')
class PreScreen(unittest.TestCase):
    def test_the_pre_screen_on_command_streams(self):
        src = (NVN / 'nvn_mtl.m').read_text()
        production = '\n'.join([
            function_body(src, 'static void rt_sub(const p_set_rt *r, int k, uint64_t *ser, int *lv, int *sl) {'),
            one_line(src, 'static int sub_eq(const p_set_rt *a, int ka, const p_set_rt *b, int kb) {'),
            function_body(src, 'static int rt_same(const p_set_rt *a, const p_set_rt *b) {'),
            function_body(src, 'static int defer_chain_pre(void) {'),
            function_body(src, 'static int defer_tex_alias(const tex_t *a, const tex_t *b) {'),
            function_body(src, 'static int defer_chain_state_op(uint32_t op) {'),
        ])
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'probe.c'
            probe.write_text(PROBE.replace('/* PRODUCTION */', production))
            exe = Path(tmp) / 'probe'
            subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wno-unused-function', '-I', str(NVN), '-I', str(NVN / 'include'),
                            '-I', str(ROOT / 'games/totk/runtime'), str(probe), '-o', str(exe)], check=True)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('pass chain pre-screen: ok', run.stdout)


class Conditions(unittest.TestCase):
    src = (NVN / 'nvn_mtl.m').read_text()
    look = function_body(src, 'static void defer_try(const p_draw *dw, const p_draw_idx *di) {')

    def test_only_without_the_other_rewrites_and_only_when_switched_on(self):
        self.assertIn('const int chain = !tbdr_on();', self.look)
        self.assertIn('if ((chain ? !defer_chain_on() : !defer_on()) || defer_has_in_p()', self.look)
        on = function_body(self.src, 'static int defer_chain_on(void) {')
        self.assertIn('getenv("TOTK_PASS_CHAIN")', on)
        oot, rest = on[on.index('#if defined(GAME_OOT) && !TARGET_OS_IPHONE'):on.index('#else')], on[on.index('#else'):on.index('#endif')]
        self.assertIn('v = !(e && !strcmp(e, "0"));', oot, 'Ocarina of Time on the Mac: on unless switched off')
        self.assertIn('v = e && !strcmp(e, "1");', rest, 'every other game, and iOS: only when asked for')
        self.assertIn('if (!g_enc && (tbdr_on() || defer_chain_on()) && !defer_has_in_p()', self.src)

    def test_the_deferral_under_tbdr_is_as_it_was(self):
        self.assertIn('else { if (!has_clear) return; if (!defer_pre()) return; }', self.look)
        replay = function_body(self.src, 'static void defer_replay(void) {')
        self.assertIn('g_pso_lmap = d->chain ? NULL : lm;', replay)
        flush = function_body(self.src, 'static void defer_flush_q(void) {')
        self.assertIn('if (g_defer_slots[i].active && g_defer_slots[i].at_q && !g_defer_slots[i].chain) return;', flush)

    def test_what_a_deferred_draw_must_be(self):
        """No depth write and plain stages from the first draw on, and it samples none of its own colours."""
        self.assertIn('if (S.ds.dw || !defer_chain_plain(S.prog, dw ? dw->ind : di ? di->ind : 1) || !defer_chain_pre()) return; }', self.look)
        self.assertIn('if (chain) { for (int i = 0; i < 4; i++) if (g_defer_slots[i].active && !g_defer_slots[i].at_q) return;', self.look, 'one chain at a time')
        self.assertIn('if (g_defer.nreads > 512 || defer_prog_hits(pg, tex, img, &g_defer.p_rt, 1, 1)) { g_defer.reason = 7; goto fail; }', self.look)
        self.assertIn('!defer_chain_plain(pg, h.op == OP_DRAW ? ((const p_draw *)a)->ind : ((const p_draw_idx *)a)->ind)', self.look)
        plain = function_body(self.src, 'static int defer_chain_plain(const prog_t *pg, uint64_t ind) {')
        self.assertIn('if (ind || !pr || pr->missing || pr->geo) return 0;', plain)
        self.assertIn('if (sh && ((sh->stage != 0 && sh->stage != 1) || pool_shader_writes(sh))) return 0;', plain)

    def test_what_the_passes_in_between_may_not_touch(self):
        """A depth attachment P samples (a shadow map drawn again), and the depth P tests."""
        self.assertIn('if (g_defer.chain && scan_rt.depth) for (int i = 0; i < g_defer.nreads && !srt_hit; i++) srt_hit = defer_handle_sub(g_defer.p_reads[i], &scan_rt, 8, 1);', self.look)
        self.assertIn('if (g_defer.p_dtest && g_defer.p_rt.depth && scan_rt.depth->serial == g_defer.p_rt.depth->serial) { g_defer.reason = 13; goto fail; }', self.look)
        self.assertIn('for (int i = 0; i < g_defer.nreads; i++) if (defer_handle_sub(g_defer.p_reads[i], &scan_rt, 8, 1)) { g_defer.reason = 12; goto fail; }', self.look)

    def test_the_draws_of_earlier_links_keep_counting(self):
        """A group that joins the next one brings what its draws read and test: a pass later in the chain may not
        overwrite the shadow map an earlier light still has to sample."""
        join = self.look[self.look.index('if (chain) {   // the groups waiting at this pass join this one'):]
        join = join[:join.index('\n    }\n') + 7]
        self.assertIn('if (!seen) { if (g_defer.nreads < 512) g_defer.p_reads[g_defer.nreads++] = u->p_reads[k]; else g_defer.nreads = 513; }', join)
        self.assertIn('g_defer.p_dtest |= u->p_dtest; g_defer.p_st |= u->p_st;', join)

    def test_q_is_the_same_pass_and_clears_nothing_first(self):
        self.assertIn('(g_defer.chain && !rt_same(r, &g_defer.p_rt))) { g_defer.reason = 6; goto fail; }', self.look)
        self.assertIn('(g_defer.chain && !rt_same(r2, &g_defer.p_rt))) { g_defer.reason = 15; goto fail; }', self.look)

    def test_q_holds_only_state_and_plain_draws(self):
        """Whatever else stood in Q's segment would run before P's replayed draws; a mesh or geometry draw opening Q
        would replay them with its own program bound (review 2026-11-18)."""
        self.assertIn('if (g_defer.chain && !defer_chain_state_op(h.op)) { g_defer.reason = 19; g_defer.reason_op = h.op; goto fail; }', self.look)
        self.assertIn('if (g_defer.chain && !defer_chain_plain(pg, h.op == OP_DRAW ? ((const p_draw *)a)->ind : ((const p_draw_idx *)a)->ind)) { g_defer.reason = 22; goto fail; }', self.look)
        self.assertIn('if (q_body && (h.op == OP_SET_RT || (h.op == OP_END && !g_defer.chain))) break;', self.look, 'the segment runs on into the next command list')

    def test_memory_not_names(self):
        """Targets between P and Q are compared with P's attachments and reads by memory; no copy, no store, no event."""
        self.assertIn('if (g_defer.chain && p_end && defer_chain_rt_conflict(&scan_rt)) { g_defer.reason = 12; goto fail; }', self.look)
        conflict = function_body(self.src, 'static int defer_chain_rt_conflict(const p_set_rt *r) {')
        self.assertIn('if (p && p->serial != t->serial && defer_tex_alias(p, t)) return 1;', conflict)
        self.assertIn('if (tid && tid < g_ntreg && g_treg[tid].ok && defer_tex_alias(&g_treg[tid].t, t)) return 1;', conflict)
        self.assertIn('case OP_COPY_T2T: { const p_copy_t2t *c = a; if (g_defer.chain) { g_defer.reason = 3;', self.look)
        self.assertIn('case OP_SIGNAL_EVENT: case OP_FENCE_SYNC: if (g_defer.chain) { g_defer.reason = 2;', self.look)
        self.assertIn('if (g_defer.chain && defer_chain_stores(pg)) { g_defer.reason = 24; goto fail; }', self.look)
        plain = function_body(self.src, 'static int defer_chain_plain(const prog_t *pg, uint64_t ind) {')
        self.assertIn('pool_shader_writes(sh)', plain, 'a draw that moves stores nothing')

    def test_a_group_holds_all_of_its_draws(self):
        """With the joined groups' draws and all of P's own: one that no longer fitted was drawn at once, before the rest."""
        self.assertIn('if (na + (g_defer.chain ? draws : 1) > 64) { g_defer.reason = 20; goto fail; }', self.look)
        self.assertIn('g_defer.p_st = chain && S.ds.st;', self.look)

    def test_a_q_that_drew_nothing_does_not_hand_the_draws_to_the_next_pass(self):
        at = self.src.index('if (g_defer_natq > 0) defer_flush_q();')
        self.assertLess(at, self.src.index('if (defer_begin_q(p)) break;'), 'before the next targets take over')
        self.assertIn('defer_flush_q();    // the pass chain: a last Q that drew nothing\n    defer_fallback();', self.src)
        flush = function_body(self.src, 'static void defer_flush_q(void) {')
        self.assertIn('R.rt = d->p_rt; R.has_rt = 1; memcpy(R.clear_c, d->clear_c, 8);', flush)
        self.assertIn('rp_flush();', flush)
        self.assertIn('memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;', flush)


if __name__ == '__main__':
    unittest.main()
