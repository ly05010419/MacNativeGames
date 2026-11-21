"""The spatial audio route decision, TOTK_SPATIAL_AUDIO, the 5.1 fold and the device-paced mixing steps
(games/totk/runtime/audio_route.h) without a device or game data."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AudioRoute(unittest.TestCase):
    def test_route_setting_fold_and_pacing(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'audio-route'
            build = subprocess.run(
                [compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                 '-fno-sanitize-recover=all', str(ROOT / 'tests/test_audio_route.c'), '-o', str(executable)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('audio route ok', run.stdout)


if __name__ == '__main__':
    unittest.main()
