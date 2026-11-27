"""The VR item wheel's icons: the slot-to-icon names (zeldaVR/runtime/oot_vr_wheel_icons.h, compiled and run natively) are
all ones the packaging script makes (games/oot/scripts/make_vr_icons.py ICONS), and the script makes nothing the wheel never
shows."""
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('make_vr_icons', ROOT / 'games/oot/scripts/make_vr_icons.py')
icons = importlib.util.module_from_spec(spec)
spec.loader.exec_module(icons)


class WheelIcons(unittest.TestCase):
    def test_the_names(self):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'icons')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                            str(ROOT / 'tests/test_oot_vr_wheel_icons.c'), '-o', exe], check=True, capture_output=True, text=True)
            out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        shown = {line.split(' ', 1)[1] for line in out.splitlines() if line.startswith('icon ')}
        self.assertIn('wheel icons: ok', out)
        self.assertEqual(shown, set(icons.ICONS))


if __name__ == '__main__':
    unittest.main()
