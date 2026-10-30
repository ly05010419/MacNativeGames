"""Motion from the HID input reports of a pad GameController gives no motion (8BitDo Ultimate 2 Wireless): runtime/hid_imu.h."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS and the Xcode command line tools')
class HidImuTests(unittest.TestCase):
    def test_decode(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'probe'
            build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{TOTK}/runtime',
                                    str(TOTK / 'tests/hid_imu/probe.c'), '-o', str(probe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr[-2000:])
            run = subprocess.run([str(probe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(json.loads(run.stdout.strip().splitlines()[-1])['status'], 'pass')


if __name__ == '__main__':
    unittest.main()
