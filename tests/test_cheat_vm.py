"""Synthetic data-format programs only; never store game instructions in fixtures."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


class CheatVMTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'C compiler required')
    def test_bounded_memory_and_control_flow(self):
        with tempfile.TemporaryDirectory() as d:
            binary = pathlib.Path(d) / 'probe'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'games/totk/runtime'),
                            str(ROOT / 'tests/cheat_vm/probe.c'), '-o', str(binary)], check=True, capture_output=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('synthetic checks passed', result.stdout)

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('cc'), 'Mach memory host requires macOS')
    def test_native_guards_ownership_restore_and_host_isolation(self):
        with tempfile.TemporaryDirectory() as d:
            binary = pathlib.Path(d) / 'native-probe'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'games/totk/runtime'),
                            str(ROOT / 'tests/cheat_vm/native_probe.c'), '-o', str(binary)], check=True, capture_output=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('guarded transactions passed', result.stdout)
