"""The launcher's update summary (games/totk/app/update_summary.h): fake network, fake AI commands, no real one of either."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation requires macOS')
class UpdateSummary(unittest.TestCase):
    def test_probe(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory).resolve() / 'probe'
            subprocess.run([compiler, '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'),
                            str(ROOT / 'tests/update_summary_probe.m'), '-framework', 'Foundation', '-o', str(probe)],
                           check=True, timeout=90)
            for language in ('zh', 'en'):
                with self.subTest(language=language):
                    run = subprocess.run([str(probe), language], capture_output=True, text=True, timeout=60)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn('pass: update summary', run.stdout)

    def test_launcher_modes(self):
        launcher = (ROOT / 'games/totk/app/launcher.m').read_text(encoding='utf-8')
        run = launcher.split('static void update_summary_run(void) {')[1]
        self.assertTrue(run.lstrip().startswith('setpriority(PRIO_PROCESS, 0, 20);'))   # started by the launcher or by the page
        self.assertIn('return mng_summary_login(mng_summary_dirs(), ^{ update_summary_run(); });', launcher)


if __name__ == '__main__':
    unittest.main()
