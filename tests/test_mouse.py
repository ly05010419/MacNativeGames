"""Mac Keyboard + Mouse mode: runtime/mouse_look.h (stick panning and gyro) and runtime/gamepad.m (buttons, ticks, saved state)."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'


def run_probe(test, command, *args):
    with tempfile.TemporaryDirectory() as tmp:
        probe, data = Path(tmp) / 'probe', Path(tmp) / 'data'
        data.mkdir()
        build = subprocess.run(command + ['-o', str(probe)], capture_output=True, text=True, timeout=120)
        test.assertEqual(build.returncode, 0, build.stderr[-2000:])
        run = subprocess.run([str(probe), *[str(data) if a == '<data>' else a for a in args]], capture_output=True, text=True, timeout=30)
        test.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        test.assertEqual(json.loads(run.stdout.strip().splitlines()[-1])['status'], 'pass')


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS and the Xcode command line tools')
class MouseTests(unittest.TestCase):
    def test_look(self):
        run_probe(self, ['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{TOTK}/runtime',
                         str(TOTK / 'tests/mouse_look/probe.c')])

    def test_mouse_mode(self):
        run_probe(self, ['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', str(TOTK / 'tests/mouse/probe.m'),
                         '-framework', 'Foundation', '-framework', 'GameController', '-framework', 'IOKit'], '<data>')


if __name__ == '__main__':
    unittest.main()
