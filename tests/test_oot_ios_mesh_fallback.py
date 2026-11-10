"""Ocarina of Time on the iPhone / iPad never compiles a mesh pipeline (user 2026-11-10: "这个是兜底 必须有", "不允许奔溃").

The pipeline archive has no mesh pipelines, and compiling them on the phone once crashed MTLCompilerService. So on the phone a
mesh draw is skipped (draw_skipped) before any mesh function is written or compiled, the warm-up leaves its M rows, and a
pipeline Metal refuses is a skip, never an exit. The Mac keeps its path. These check the source, as the iOS build is not run
here.
"""
import re
import unittest
from pathlib import Path

MESH = Path(__file__).resolve().parents[1] / 'games/oot/third_party/d2r_gfx/gfx/nvn/nvn_mesh_tasks.inc'


def function(source, name):
    start = source.index(name + '(')
    start = source.rindex('\n', 0, start) + 1
    end = source.index('\n}\n', start)
    return source[start:end]


class MeshFallback(unittest.TestCase):
    source = MESH.read_text()

    def test_switch_defaults_to_skip_on_the_phone_and_keeps_the_mac(self):
        body = function(self.source, 'static int mtk_device_compiles')
        self.assertIn('getenv("OOT_IOS_MESH_COMPILE")', body)
        phone, mac = body.split('#else')
        self.assertIn('#if TARGET_OS_IPHONE', phone)
        self.assertIn('return 1;', mac)

    def test_draw_is_skipped_before_any_mesh_work(self):
        body = function(self.source, 'static void mesh_tasks_draw')
        skip = body.index('if(!mtk_device_compiles()){mtk_note_skip(ms);draw_skipped();return;}')
        self.assertLess(skip, body.index('g_mtk.on=1'))
        self.assertLess(body.rindex('#if TARGET_OS_IPHONE', 0, skip), skip)

    def test_warm_up_leaves_the_m_rows_first(self):
        body = function(self.source, 'static void mtk_warm_row')
        self.assertLess(body.index('if(!mtk_device_compiles())'), body.index('mtk_build('))

    def test_a_refused_pipeline_is_a_skip_on_the_phone(self):
        body = function(self.source, 'static id<MTLRenderPipelineState> mtk_pso')
        phone = re.search(r'#if TARGET_OS_IPHONE\n\s*if\(!ps\)\{[^\n]*return nil;\}', body)
        self.assertIsNotNone(phone)
        self.assertLess(phone.start(), body.index('MDIE("task/mesh pipeline'))


if __name__ == '__main__':
    unittest.main()
