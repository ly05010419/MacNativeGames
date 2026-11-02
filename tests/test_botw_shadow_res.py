"""Breath of the Wild 1.9.0's sun shadow map at 2048 (games/totk/runtime/botw_shadow_res.h) without the game: the
words, TOTK_SHADOW_RES / shadows.txt and the patch on a fake main (tests/test_botw_shadow_res.c)."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ShadowRes(unittest.TestCase):
    def test_shadow_res(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'shadow_res'
            build = subprocess.run(
                [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                 str(ROOT / 'tests/test_botw_shadow_res.c'), '-o', str(executable)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('shadow res ok', run.stdout)

    def test_the_runtime_calls_it_only_on_190(self):
        text = (ROOT / 'games/totk/runtime/game_patches_botw.c').read_text()
        self.assertIn('#include "botw_shadow_res.h"', text)
        start = text.index('void runtime_game_patches(Module *m) {')
        body = text[start:text.index('\n}\n', start)]
        self.assertIn('#ifdef GAME_BOTW190\n    patch_shadow_res(m);\n#endif', body)


if __name__ == '__main__':
    unittest.main()
