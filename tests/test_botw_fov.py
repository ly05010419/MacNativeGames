"""Breath of the Wild 1.9.0's field of view (games/totk/runtime/botw_fov.h) without the game: graphics-fov.txt and
TOTK_FOV_SCALE, the stub's words and the patch on a fake main, with the stub run by a small interpreter
(tests/test_botw_fov.c), next to the ultrawide writer's words as game_patches_botw.c has them."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games/totk/runtime'


def function(text, head):
    start = text.index(head)
    return text[start:text.index('\n}\n', start)]


class BotwFov(unittest.TestCase):
    def test_fov(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        source = (RUNTIME / 'game_patches_botw.c').read_text()
        at = source.index('#define UW_WRITER_AT 0x10edae0u')   # 1.9.0's writer (1.6.0's comes first)
        words = re.search(r'uw_writer_original\[\] = \{([^}]*)\}', source[at:]).group(1)
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'botw_fov'
            build = subprocess.run(
                [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-DUW_ORIGINAL=' + ' '.join(words.split()),
                 str(ROOT / 'tests/test_botw_fov.c'), '-o', str(executable)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('botw fov ok', run.stdout)

    def test_the_runtime_calls_it_only_on_190_and_the_mac(self):
        text = (RUNTIME / 'game_patches_botw.c').read_text()
        self.assertIn('#include "botw_fov.h"', text)
        body = function(text, 'void runtime_game_patches(Module *m) {')
        self.assertIn('#ifdef GAME_BOTW190\n    patch_fov(m);\n#endif', body)
        patch = function(text, 'static void patch_fov(Module *m) {')
        self.assertEqual(patch.split('\n')[1].strip(), 'if (ios_code()) return;')
        start = text.index('static _Atomic uint32_t *g_fov_factor;')
        self.assertEqual(text.rfind('#ifdef GAME_BOTW190', 0, start), text.rfind('#if', 0, start))

    def test_a_mismatch_never_stops_the_game(self):
        text = (RUNTIME / 'game_patches_botw.c').read_text()
        start = text.index('static _Atomic uint32_t *g_fov_factor;')
        code = text[start:text.index('#endif', start)] + (RUNTIME / 'botw_fov.h').read_text()
        for name in ('runtime_fail', 'adrp_reg', 'branch_to', 'put_word', 'abort'):
            self.assertNotIn(name + '(', code)
        self.assertIn('if (bad) {\n        g_fov_refused = 1;', text)   # the row goes unavailable

    def test_the_factor_is_written_before_the_branch(self):
        apply = function((RUNTIME / 'botw_fov.h').read_text(), 'static inline int fov_apply(')
        order = [apply.index(s) for s in ('memcpy(main + data + FOV_FACTOR_SLOT, &bits, 4);',
                                          'memcpy(main + stub, w, sizeof w);', 'memcpy(main + FOV_SITE, &jump, 4);')]
        self.assertEqual(order, sorted(order))   # the factor, the stub, then the branch: no frame reads a factor of 0


if __name__ == '__main__':
    unittest.main()
