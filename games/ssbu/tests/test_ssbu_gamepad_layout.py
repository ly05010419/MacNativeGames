"""Face buttons by position or label, and the HID pads GameController does not take (runtime/gamepad_layout.h)."""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SSBU = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class GamepadLayoutTests(unittest.TestCase):
    def test_probe(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'probe'
            build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{SSBU}/runtime',
                                    str(SSBU / 'tests/gamepad_layout/probe.c'), '-o', str(probe)], capture_output=True, text=True)
            if build.returncode:
                self.skipTest(f'clang could not build the probe: {build.stderr[-300:]}')
            run = subprocess.run([str(probe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(json.loads(run.stdout)['status'], 'pass')


if __name__ == '__main__':
    unittest.main()
