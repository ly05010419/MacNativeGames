"""The shared hook body of Ocarina of Time's VR and its stubs, run natively (games/oot/runtime/oot_vr_hooks.h)."""
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64', 'the words are arm64 code')
class Hooks(unittest.TestCase):
    def test_the_words_run(self):
        tmp = tempfile.mkdtemp()
        try:
            done = subprocess.run([sys.executable, '-B', str(ROOT / 'games/oot/tests/oot_vr_hooks_native.py'), '--work-dir', tmp],
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn('makes the chained calls into the game', done.stdout)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
