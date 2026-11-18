"""#2037 / #2039 / #2042: the movies' network replaced by MetalFX, always (the user: "默认开，不用设置控制"; not the
settings panel's upscaler switch): the recognition by data flow (oot_movie_bypass.h) and its use in the replay."""
from pathlib import Path
import subprocess
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
NVN = GAME / 'third_party/d2r_gfx/gfx/nvn'


class MovieBypassTests(unittest.TestCase):
    def test_recognition(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'test'
            build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', str(GAME / 'tests/movie_bypass_test.c'), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('all checks pass', run.stdout)

    def test_the_replay_uses_it_whatever_the_upscaler_setting(self):
        inc = (NVN / 'oot_net_bypass.inc').read_text()
        self.assertIn('#include "oot_movie_bypass.h"', inc)
        self.assertIn('netbp_movie(cs)', inc)
        movie = inc[inc.index('static int netbp_movie('):inc.index('static int netbp_dispatch(')]
        self.assertNotIn('netbp_on()', movie)   # the settings panel's upscaler does not turn it off
        self.assertIn('if (!netbp_movie_on())', movie)
        self.assertIn('getenv("OOT_MOVIE_BYPASS")', inc)   # =0: the game's network, for comparisons only
        mtl = (NVN / 'nvn_mtl.m').read_text()
        body = mtl[mtl.index('static void copy_b2t(const p_copy_b2t *c) {'):]
        self.assertIn('netbp_note_upload(c->tex->serial)', body[:body.index('\n}\n')])


if __name__ == '__main__':
    unittest.main()
