"""NVN semantics ported from seedds/nxnative (MIT) pull requests into the shared graphics layer (games/totk/third_party/d2r_gfx,
which every game builds from since the unification). Each test pins one port, so a later edit cannot drop it unseen."""
import unittest
from pathlib import Path

NVN = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn'


def function_body(src, signature):
    start = src.index(signature)
    return src[start:src.index('\n}\n', start)]


class StencilBackReference(unittest.TestCase):
    """#2351 (2fb82008): each face has its own stencil reference value; the back face's reaches Metal."""

    def test_both_faces_reach_metal(self):
        src = (NVN / 'nvn_mtl.m').read_text()
        line = next(l for l in src.splitlines() if l.startswith('static inline void enc_r_stencil_ref('))
        self.assertNotIn('(void)b', line)
        self.assertIn('setStencilFrontReferenceValue:f backReferenceValue:b', line)
        self.assertIn('.u={f, b}', line, 'the encode thread gets both values')
        self.assertIn('g_es.sref_b == b', line, 'the state cache compares both')
        stream = (NVN / 'nvn_render_stream.h').read_text()
        self.assertIn('case RS_STENCIL: [e setStencilFrontReferenceValue:(uint32_t)u[0] backReferenceValue:(uint32_t)u[1]]', stream)
        self.assertIn('enc_r_stencil_ref(en, S.st_ref[0], S.st_ref[1])', src)


class IndirectDispatchSplit(unittest.TestCase):
    """#2226: an indirect dispatch ends a copy encoder that already holds work, so its group counts are read after the
    dispatch that wrote them (Ocarina of Time by default; DM_NVN_IND_SPLIT=0/1 for any game)."""

    def test_the_split_comes_before_the_encoder_is_taken(self):
        src = (NVN / 'nvn_mtl.m').read_text()
        body = function_body(src, 'static void dispatch(const p_dispatch *dp) {')
        split = body.index('if (reuse && dp->indirect && g_blit && ind_split())')
        self.assertLess(split, body.index('ce = disp_enc('), 'before the encoder is taken (disp_enc, #2250)')
        self.assertIn('enc_b_end(g_blit); g_blit = nil;', body[split:split + 300])
        rule = function_body(src, 'static int ind_split(void) {')
        oot, other = rule.split('#else', 1)
        self.assertIn('#if defined(GAME_OOT)', oot)
        self.assertIn('v = !(e && !strcmp(e, "0"));', oot, 'on for Ocarina of Time unless turned off')
        self.assertIn('v = e && atoi(e) != 0;', other, 'off for every other game unless turned on')


if __name__ == '__main__':
    unittest.main()
