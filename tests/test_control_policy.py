"""CPU-only render-work and pacing policy checks, without player data."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

class ControlPolicy(unittest.TestCase):
    def test_render_work_and_pacing(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler required')
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'controls'
            source = Path(__file__).with_suffix('.c')
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('PASS:', result.stdout)
