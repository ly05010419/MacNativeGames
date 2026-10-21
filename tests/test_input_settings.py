"""Compile the production AppKit editor; run with TOTK_RUN_APPKIT_PROBES=1 on a logged-in Mac."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

@unittest.skipUnless(sys.platform == 'darwin', 'AppKit requires macOS')
class InputSettings(unittest.TestCase):
    def test_binding_editor(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / 'input-settings'
            command = ['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                       '-Wno-deprecated-declarations', '-I' + str(root / 'games/totk/third_party/d2r_gfx/gfx/nvn/include'),
                       str(root / 'tests/input_settings_probe.m'), '-framework', 'Cocoa',
                       '-framework', 'Metal', '-framework', 'QuartzCore', '-o', str(exe)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if os.environ.get('TOTK_RUN_APPKIT_PROBES') == '1':
                run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('PASS:', run.stdout)

    def test_gamepad_state_transitions(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / 'gamepad'
            result = subprocess.run(['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                     str(root / 'tests/gamepad_bindings_probe.m'), '-framework', 'Foundation',
                                     '-framework', 'GameController', '-o', str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            env = {k: v for k, v in os.environ.items() if k not in ('TOTK_CONTROLS', 'TOTK_FACE_BUTTONS')}
            run = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS:', run.stdout)
