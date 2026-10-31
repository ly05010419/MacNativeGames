"""Controller type on key 6 (runtime/gamepad.m): cycle, saved next to controls.txt, read back at the next start."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS and the Xcode command line tools')
class GamepadTypeTests(unittest.TestCase):
    def test_probe(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe, data = Path(tmp) / 'probe', Path(tmp) / 'data'
            data.mkdir()
            build = subprocess.run(['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                    str(TOTK / 'tests/gamepad_type/probe.m'), '-framework', 'AppKit', '-framework', 'Foundation',
                                    '-framework', 'GameController', '-framework', 'IOKit', '-o', str(probe)],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr[-2000:])
            run = subprocess.run([str(probe), str(data)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(json.loads(run.stdout.strip().splitlines()[-1])['status'], 'pass')


if __name__ == '__main__':
    unittest.main()
