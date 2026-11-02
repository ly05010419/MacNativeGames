"""iPhone / iPad motion aiming: ios/Sources/device_motion_axes.h (device frame -> the controller's six-axis frame)."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'


def run_probe(test, command):
    with tempfile.TemporaryDirectory() as tmp:
        probe = Path(tmp) / 'probe'
        build = subprocess.run(command + ['-o', str(probe)], capture_output=True, text=True, timeout=120)
        test.assertEqual(build.returncode, 0, build.stderr[-2000:])
        run = subprocess.run([str(probe)], capture_output=True, text=True, timeout=30)
        test.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        test.assertEqual(json.loads(run.stdout.strip().splitlines()[-1])['status'], 'pass')


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS and the Xcode command line tools')
class DeviceMotionTests(unittest.TestCase):
    def test_axes(self):
        run_probe(self, ['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{TOTK}/ios/Sources',
                         str(TOTK / 'tests/device_motion/probe.c')])


if __name__ == '__main__':
    unittest.main()
