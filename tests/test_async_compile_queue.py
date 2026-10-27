"""Check compiler scheduling without game data; smoke-test Metal when available."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'Metal requires macOS')
class AsyncCompileQueue(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang')
        if not compiler:
            raise unittest.SkipTest('clang required')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.executable = Path(cls.directory.name) / 'compile-queue'
        build = subprocess.run([compiler, '-std=gnu11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                str(ROOT / 'tests/async_compile_probe.m'), '-framework', 'Foundation',
                                '-framework', 'Metal', '-o', str(cls.executable)], capture_output=True, text=True)
        if build.returncode:
            raise AssertionError(build.stderr)

    def test_device_limit_and_nonblocking_submission(self):
        for limit in ('2', '0'):
            with self.subTest(device_limit=limit):
                run = subprocess.run([str(self.executable), limit], capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('pass: bounded', run.stdout)

    def test_real_metal_background_compilation(self):
        run = subprocess.run([str(self.executable), 'metal'], capture_output=True, text=True, timeout=60)
        if run.returncode == 77:
            self.skipTest(run.stdout.strip())
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('pass: background Metal', run.stdout)


if __name__ == '__main__':
    unittest.main()
