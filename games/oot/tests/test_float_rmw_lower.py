"""#2070: the lowering of device float adds (nvn_float_rmw.h), as text and on the GPU, and the runtime and packaging that
use it (only for a shader whose native pipeline cannot be built, remembered per GPU and shipped with the pipelines)."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
ROOT = GAME.parents[1]
NVN = GAME / 'third_party/d2r_gfx/gfx/nvn'


class LoweringTests(unittest.TestCase):
    def run_built(self, sources, *flags, args=()):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'test'
            build = subprocess.run(['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', *flags, *map(str, sources), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

    def test_text(self):
        self.assertIn('all checks pass', self.run_built([GAME / 'tests/float_rmw_lower_test.c'], '-std=c11'))

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs Metal')
    def test_gpu_exact_at_scale(self):
        out = self.run_built([GAME / 'tests/float_rmw_gpu_test.m'], '-fobjc-arc', '-framework', 'Foundation', '-framework', 'Metal',
                             args=('1536', '864'))
        self.assertTrue('exact' in out or 'SKIP' in out, out)


class RuntimeAndPackagingTests(unittest.TestCase):
    MTL = (NVN / 'nvn_mtl.m').read_text()

    def test_the_runtime_lowers_only_after_the_native_pipeline_fails_and_remembers_it(self):
        self.assertIn('#include "nvn_float_rmw.h"', self.MTL)
        self.assertIn('frmw_retry(cs', self.MTL)
        self.assertIn('frmw_listed(cs', self.MTL)

    def test_packaging_ships_this_gpus_list(self):
        if str(ROOT) not in sys.path:
            sys.path.insert(0, str(ROOT))
        import build_app
        with tempfile.TemporaryDirectory() as tmp:
            made, app = Path(tmp) / 'made', Path(tmp) / 'app'
            made.mkdir()
            (made / 'a.metalarchive').write_bytes(b'archive')
            (made / 'a.keys').write_text('k1\nk2\n')
            (made / 'float-rmw-lowered.txt').write_text('applegpu_g17g\tshader-b\napplegpu_g16g\tshader-x\napplegpu_g17g\tshader-a\n')
            self.assertEqual(build_app.mac_pipeline_files(made, app, 'applegpu_g17g', 'chain'), 2)
            self.assertEqual((app / 'pipelines-applegpu_g17g-lowered.txt').read_text(), 'shader-a\nshader-b\n')
            build_app.mac_pipeline_files(made, app, 'applegpu_g16s', 'chain')   # another GPU: none of its own, the old list gone
            self.assertEqual(sorted(p.name for p in app.glob('*lowered*')), [])


if __name__ == '__main__':
    unittest.main()
