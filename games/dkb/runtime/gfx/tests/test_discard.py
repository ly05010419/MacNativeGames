"""Discard entry points preserve vertex side effects and compile on both platforms."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

path = Path(__file__).resolve().parents[1] / 'tools/dmsh_build.py'
nodes = [n for n in ast.parse(path.read_text()).body if isinstance(n, ast.FunctionDef) and n.name in ('vs_discard', 'vs_writes')]
scope = {'re': re}
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(path), 'exec'), scope)

SOURCE = '''#include <metal_stdlib>
using namespace metal;
struct main0_out { float4 p [[position]]; };
struct Args { device uint* values [[id(0)]]; };
vertex main0_out main0(uint n [[vertex_id]], constant Args& a [[buffer(0)]])
{
    main0_out out = {};
    a.values[n] = n + 17;
    if (n == 0) return out;
    a.values[n] = n + 29;
    return out;
}
'''


class DiscardTests(unittest.TestCase):
    def test_storage_writes_need_a_discard_entry(self):
        self.assertTrue(scope['vs_writes'](SOURCE, [{'kind': 'sbuf', 'rw': True}]))

    def test_image_stores_need_a_discard_entry(self):   # Metroid Prime Remastered's fog volume (1cbc19f806a08166): points write a 3D image
        src = 'struct spvDescriptorSetBuffer0 { texture3d<float, access::write> vp_i_8 [[id(0)]]; };'
        self.assertTrue(scope['vs_writes'](src, [{'kind': 'img', 'reg': 0}]))

    def test_reads_only_need_none(self):
        src = 'struct spvDescriptorSetBuffer0 { texture3d<float> vp_t_tcb_8 [[id(0)]]; device const uint* vp_s0 [[id(1)]]; };'
        self.assertFalse(scope['vs_writes'](src, [{'kind': 'sbuf', 'rw': False}, {'kind': 'tex', 'reg': 0}]))

    @unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
    def test_side_effects_before_both_returns_compile_as_void_vertex(self):
        src = SOURCE + scope['vs_discard'](SOURCE)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); msl = root / 'discard.metal'; msl.write_text(src)
            for sdk in ('macosx', 'iphoneos'):
                with self.subTest(sdk=sdk):
                    r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fmodules-cache-path=' + str(root / sdk), str(msl), '-o', str(root / (sdk + '.air'))], capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
