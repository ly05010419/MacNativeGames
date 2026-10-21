"""Production keyboard-dispatch regression checks; no game data or UI."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

@unittest.skipUnless(sys.platform == 'darwin', 'Darwin runtime input ABI')
class ControlInput(unittest.TestCase):
    def test_exact_shortcut_dispatch(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('Clang required')
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'input'
            source = Path(__file__).with_suffix('.c')
            compiled = subprocess.run([compiler, '-std=c11', '-ffunction-sections', '-fdata-sections',
                            '-Wl,-dead_strip', str(source), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            env = {k: v for k, v in os.environ.items() if k not in ('TOTK_INPUT_FILE', 'TOTK_INPUT_SCRIPT')}
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True, env=env)
            self.assertIn('PASS:', result.stdout)
