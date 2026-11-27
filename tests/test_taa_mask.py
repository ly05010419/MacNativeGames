"""Edge smoothing's reactive mask kernels (taa_kernels.h), run on the GPU on synthetic depth (tests/taa_mask_probe.m).
The mask decides where MetalFX and the fast resolve throw their history away: on a still scene it must stay 0 (or the
anti-aliasing is lost), where a surface moved in it must reach 1 (or the old surface trails). The kernels are compiled
from the same header totk_taa.m compiles, so the probe tests what the game runs."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


class Kernels(unittest.TestCase):
    def test_the_game_compiles_the_shared_header(self):
        taa = (GFX / 'totk_taa.m').read_text()
        self.assertIn('#include "taa_kernels.h"', taa)
        self.assertIn('@(taa_kernels_msl)', taa)
        self.assertNotIn('kernel void taa_mv(', taa)   # one copy of the kernel: the header's


@unittest.skipUnless(sys.platform == 'darwin', 'Metal runs on macOS')
class MaskProbe(unittest.TestCase):
    def test_probe(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'taa-mask'
            build = subprocess.run(
                [compiler, '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-I' + str(GFX), str(ROOT / 'tests/taa_mask_probe.m'),
                 '-framework', 'Foundation', '-framework', 'Metal', '-o', str(executable)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            if run.returncode == 77:
                self.skipTest('no Metal device')
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            for case in ('static_scene_zero', 'jump_20pct_full', 'jump_7pct_partial', 'prev_invalid_zero',
                         'thin_strand_1px_shift_not_masked_known_limit', 'static_layer_pan_zero',
                         'moving_particle_masks_both_spots', 'no_transparents_zero', 'reset_gives_no_layer_mask', 'jittered_fog_edge_zero'):
                self.assertIn(case + ' ok', run.stdout)


MTL = (GFX / 'nvn_mtl.m').read_text()
TAA = (GFX / 'totk_taa.m').read_text()


def body(text, start):
    i = text.index(start)
    return text[i:text.index('\n}\n', i)]


def real(text):   # totk_taa.m: the visionOS stubs first, the real code after their #else
    return text[text.index('#else', text.index('#if TARGET_OS_VISION')):]


class Hooks(unittest.TestCase):
    """Slice 2 (2026-11-25): the HDR scene is copied before the first pass that blends into it (learned per frame), and
    taa_encode turns the changing part of what those passes added into mask."""

    def test_scene_matched_through_views(self):
        # the compose draw samples a view of the scene; the passes render into its parent (BotW 1.9.0, 2026-11-25 log):
        # both sides are compared by their root texture, or no scene pass is ever found
        lit = body(real(TAA), 'id<MTLTexture> taa_lit_pass(MTL4RenderPassDescriptor *pd) {')
        self.assertIn('c0.parentTexture ?: c0', lit)
        enc = body(real(TAA), 'id<MTLTexture> taa_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src) {')
        self.assertIn('g.scene_root = (__bridge void *)(src.parentTexture ?: src);', enc)

    def test_last_src_never_messaged_at_pass_open(self):
        # g.last_src is unretained: the descriptor's view can be freed during frames without anti-aliasing (loading, a
        # resolution change); messaging it at the next pass open would crash. Only an opaque pointer is compared.
        lit = body(real(TAA), 'id<MTLTexture> taa_lit_pass(MTL4RenderPassDescriptor *pd) {')
        self.assertNotIn('g.last_src', lit)
        self.assertIn('(__bridge void *)(c0.parentTexture ?: c0) != g.scene_root', lit)
        self.assertRegex(TAA, r'void \*scene_root;')

    def test_layer_on_only_in_botw_190_on_the_mac(self):
        # TotK, iOS and zeldaVR (1.6.0) keep their picture (spec constraint): the layer defaults off outside BotW 1.9.0 Mac
        on = body(real(TAA), 'static int layer_on(void) {')
        self.assertIn('#if defined(GAME_BOTW190) && !TARGET_OS_IPHONE', on)

    def test_lit_copy_only_when_taa_will_run(self):
        self.assertIn('taa_ready()', body(real(TAA), 'id<MTLTexture> taa_lit_pass(MTL4RenderPassDescriptor *pd) {'))

    def test_copy_at_pass_open_with_the_replays_blit(self):
        i = MTL.index('taa_lit_pass(pd)')
        self.assertLess(MTL.index('if (taa_pass(pd) == 2)'), i)
        call = MTL[i:MTL.index('\n', i)]
        self.assertIn('blit()', call); self.assertIn('enc_b_copy_texture(', call); self.assertIn('blit_close();', call)

    def test_blend_watched_after_the_pass_opens(self):
        i = MTL.index('taa_lit_blend()')
        self.assertLess(MTL.index('g_enc ? g_enc : rp_begin_ex(mode);'), i)
        self.assertIn('S.color.blend_en & 1', MTL[MTL.rindex('\n', 0, i):MTL.index('\n', i)])

    def test_mismatched_lit_snapshot_is_skipped(self):
        enc = body(real(TAA), 'id<MTLTexture> taa_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src) {')
        cond = enc[enc.index('int layer = '):enc.index(';', enc.index('int layer = '))]
        for s in ('g.lit_frame == g.frame', 'g.lit.width == src.width', 'g.lit.height == src.height', 'g.lit.pixelFormat == src.pixelFormat'):
            self.assertIn(s, cond)

    def test_layer_mask_before_both_resolves(self):
        enc = body(real(TAA), 'id<MTLTexture> taa_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src) {')
        d = enc.index('g.layer_ps')
        self.assertLess(d, enc.index('if (fast) {'))
        self.assertLess(d, enc.index('encodeToCommandBuffer'))

    def test_layer_env_switch(self):
        self.assertIn('getenv("TOTK_TAA_LAYER")', TAA)
        self.assertIn('"TOTK_TAA_LAYER_THRESHOLD"', TAA)

    def test_learned_at_frame_end(self):
        end = body(real(TAA), 'void taa_frame_end(void) {')
        self.assertIn('g.lit_first >= 0', end)


if __name__ == '__main__':
    unittest.main()
