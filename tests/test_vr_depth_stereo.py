"""zeldaVR depth stereo on the GPU (tests/test_vr_depth_stereo.m): the depth capture ring and the eye passes move a near
object by its disparity in each eye without stretching it (2026-11-22, the Quest: "人物拉伸")."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == 'Darwin', 'Mac native VR host')
class DepthStereo(unittest.TestCase):
    def test_object_moves_by_its_disparity_without_stretching(self):
        with tempfile.TemporaryDirectory(prefix='mng-depth-stereo-') as folder:
            binary = Path(folder) / 'depth-stereo'
            built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                    '-framework', 'Foundation', '-framework', 'Metal',
                                    str(ROOT/'tests/test_vr_depth_stereo.m'), str(ROOT/'zeldaVR/gfx/vr_depth_stereo.m'), '-o', str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('depth stereo passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
