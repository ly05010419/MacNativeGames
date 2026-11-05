"""Exercise the independent native renderer and eye-frame ownership without a game/GPU."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == 'Darwin', 'Mac native VR host')
class LaboHost(unittest.TestCase):
    def test_hud_binocular_rays(self):
        with tempfile.TemporaryDirectory(prefix='mng-hud-rays-') as folder:
            binary = Path(folder) / 'hud-rays'
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(ROOT/'tests/test_vr_ui_rays.c'), '-o', str(binary)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn('binocular rays passed', result.stdout)
            import os
            negative = subprocess.run([str(binary)], env=dict(os.environ, TOTK_PROBE_NEGATIVE='1'), capture_output=True, text=True)
            self.assertNotEqual(negative.returncode, 0)
            self.assertIn('HUD binocular ray mismatch', negative.stderr)

    def test_renderer_policy_and_complete_eye_pairs(self):
        with tempfile.TemporaryDirectory(prefix='mng-labo-test-') as folder:
            binary = Path(folder) / 'labo-test'
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-DGAME_BOTW=1',
                            str(ROOT/'tests/test_vr_labo.c'), str(ROOT/'zeldaVR/gfx/vr_labo.c'),
                            str(ROOT/'games/totk/runtime/shared_font.c'), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn('passed', result.stdout)

    def test_projection_resource_ownership(self):
        with tempfile.TemporaryDirectory(prefix='mng-projection-test-') as folder:
            binary = Path(folder) / 'projection-test'
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                            '-framework', 'Foundation', '-framework', 'Metal',
                            str(ROOT/'tests/test_vr_projection_graph.m'),
                            str(ROOT/'zeldaVR/gfx/vr_projection_graph.m'), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn('passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
