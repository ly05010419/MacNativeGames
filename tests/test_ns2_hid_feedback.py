"""SDK 22 vibration queries route through the host with the public/detail ABI."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'macOS runtime headers')
class NS2Feedback(unittest.TestCase):
    def test_mounted_query_abi_and_routing(self):
        with tempfile.TemporaryDirectory(prefix='mng-ns2-hid-') as folder:
            env = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
            for ns2 in (False, True):
                with self.subTest(ns2=ns2):
                    exe = Path(folder) / ('ns2' if ns2 else 'switch1')
                    cmd = ['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                           '-fsanitize=undefined', '-fno-sanitize-recover=all',
                           '-I'+str(ROOT / 'games/totk/runtime'),
                           str(ROOT / 'tests/ns2_hid_feedback_probe.c'),
                           str(ROOT / 'games/totk/runtime/hid_feedback.c'), '-o', str(exe)]
                    if ns2:
                        cmd.append('-DGAME_NS2=1')
                    build = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    run = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=5)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn('pass: NS2 vibration mounted query ABI and routing', run.stdout)
                    if ns2:
                        for mode, message in [('null-output', 'vibration mounted pointer'),
                                              ('null-handle', 'vibration device handle pointer')]:
                            bad = subprocess.run([str(exe), mode], env=env, capture_output=True, text=True, timeout=5)
                            self.assertEqual(bad.returncode, 77, bad.stdout + bad.stderr)
                            self.assertIn(message, bad.stderr)


if __name__ == '__main__':
    unittest.main()
