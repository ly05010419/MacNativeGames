"""Key 3's picture mode is kept between runs (TOTK_PICTURE_MODE): the production code on a synthetic data page."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'the runtime builds on macOS')
class PictureMode(unittest.TestCase):
    def test_kept_between_runs(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'picture-mode'
            build = subprocess.run(
                [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-DTOTK_GAME_V143=1',
                 '-I' + str(ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/include'), '-Wl,-undefined,dynamic_lookup',
                 '-Wl,-dead_strip', str(ROOT / 'tests/picture_mode_probe.c'), '-o', str(executable)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable), str(Path(directory) / 'picture-mode.txt')],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pass: picture mode kept between runs', run.stdout)

    def test_the_graphics_layer_needs_nothing_of_one_runtime(self):
        """nvn_mtl.m is linked into every game's runtime; the 4K-to-5K preset reaches it as a flag. A weak_import of a
        function only Tears of the Kingdom 1.4.3 defines (PR #2240 as opened) left the other runtimes without it:
        ld "Undefined symbols" (their link lines have no -undefined dynamic_lookup)."""
        nvn = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        self.assertNotIn('runtime_metalfx_5k_output', nvn)
        self.assertIn('if (!strcmp(name, "mfx_5k_output"))', nvn)
        v143 = (ROOT / 'games/totk/runtime/game_patches_v143.c').read_text()
        self.assertIn('static int runtime_metalfx_5k_output(void)', v143)
        self.assertIn('dm_gfx_set_flag("mfx_5k_output",on!=0)', v143)


if __name__ == '__main__':
    unittest.main()
