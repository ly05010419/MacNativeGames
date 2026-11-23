"""The present pacer (nvn_pace.h): vsync semantics for a caller that waits, and the beat following frames the display link
paced (after nxnative b010f2b8), so a silent link's fallback never sleeps a lead the beat built up. No GPU, no game files."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
NVN = ROOT / 'shared/third_party/d2r_gfx/gfx/nvn'


class Pacer(unittest.TestCase):
    def test_follow_and_wait(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 't_pace'
            build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(NVN),
                                    str(ROOT / 'tests/t_pace.c'), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pacer passed', run.stdout)

    def test_acquire_marks_link_paced_frames(self):
        source = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c').read_text(encoding='utf-8')
        self.assertIn('o->follow = 0;', source)
        self.assertIn('o->follow = pace_follow_on();', source)
        self.assertIn('if (o->follow) nvn_pace_follow(&pc, now); else slot += nvn_pace(&pc, now, period);', source)


if __name__ == '__main__':
    unittest.main()
