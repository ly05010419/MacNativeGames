"""The 1.2.1 external image lookup uses size only, retaining the hash pin check."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('clang'), 'macOS launcher')
class LauncherSizeTests(unittest.TestCase):
    def test_size_and_hash_pin(self):
        game = ROOT / 'games/totk'
        with tempfile.TemporaryDirectory() as d:
            binary = Path(d) / 'launcher-probe'
            command = ['clang', '-fobjc-arc', '-O2', '-Wall', '-DTOTK_GAME_V121=1',
                       '-I' + str(game / 'third_party/d2r_gfx/gfx/nvn'), '-I' + str(game / 'runtime'),
                       '-I' + str(game / 'app'), str(ROOT / 'tests/cheat_vm/launcher_probe.m')]
            for framework in ('AppKit', 'GameController', 'IOKit', 'Foundation'):
                command += ['-framework', framework]
            subprocess.run(command + ['-o', str(binary)], check=True, capture_output=True, text=True, timeout=60)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('pass: launcher size and image-pin checks', result.stdout)
