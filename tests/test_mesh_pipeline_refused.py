"""A mesh pipeline Metal refuses skips its draw instead of ending the game, and is not compiled again in the same run.

This is a patch, not the root fix (#2107, #2310; user 2026-11-20 "3补丁"): macOS 27's AGX compiler crashes on a few Ocarina
of Time mesh pipelines on M1 / M2, and the Mac ended the game at the first draw needing one (a later save could never be
played on). The phone already skipped the draw; it now does on the Mac too, and the refused key is kept, because every
attempt takes the compiler service down for seconds. TOTK_MESH_FAIL_DIE=1 brings the exit back on the Mac."""
from pathlib import Path
import re
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mesh_tasks.inc'


def function(text, name):
    start = text.index(f'static id<MTLRenderPipelineState> {name}(')
    end = text.index('\n}\n', start)
    return text[start:end]


class RefusedMeshPipeline(unittest.TestCase):
    def setUp(self):
        self.body = function(SOURCE.read_text(), 'mtk_pso')

    def test_a_refused_key_is_skipped_before_compiling(self):
        refused = self.body.index('nvn_hm_get(&g_mtk_failed,&key)')
        self.assertLess(refused, self.body.index('mtk_build('))
        self.assertIn('return nil', self.body[refused:self.body.index('\n', refused)])

    def test_a_refusal_is_kept_and_the_draw_skipped(self):
        block = self.body[self.body.index('if(!ps){'):]
        block = block[:block.index('return nil;') + len('return nil;')]
        self.assertIn('nvn_hm_put(&g_mtk_failed,&key,', block)
        # the exit only on the Mac and only when asked for
        for die in re.finditer(r'MDIE\(', block):
            before = block[:die.start()]
            self.assertIn('#if !TARGET_OS_IPHONE', before)
            self.assertIn('if(die)', before[before.rindex('\n'):] + block[die.start() - 10:die.start()])
        self.assertIn('TOTK_MESH_FAIL_DIE', block)

    def test_no_exit_outside_the_switch(self):
        """No unconditional MDIE on a nil pipeline left in mtk_pso (the 1.x behaviour on the Mac)."""
        self.assertNotRegex(self.body, r'\n\s*if\(!ps\)MDIE\(')


if __name__ == '__main__':
    unittest.main()
