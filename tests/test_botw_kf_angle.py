"""Execute the moving-platform veneer on the native CPU."""
import json
from pathlib import Path
import platform
import subprocess
import sys
import unittest


@unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64', 'native Apple silicon probe')
class MovingPlatformAngle(unittest.TestCase):
    def test_small_turns_and_normal_turns(self):
        script = Path(__file__).resolve().parents[1] / 'games/botw/scripts/verify_kf_angle_patch.py'
        result = subprocess.run([sys.executable, str(script)], check=True, capture_output=True, text=True)
        report = json.loads(result.stdout)
        self.assertEqual(report['status'], 'pass')
        self.assertGreater(report['checks'], 200000)
        self.assertGreater(report['old_zero_small_angles'], 0)


if __name__ == '__main__':
    unittest.main()
