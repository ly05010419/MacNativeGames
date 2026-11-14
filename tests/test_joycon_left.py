"""Paired Joy-Con left input must not depend on the motion or rumble owner (fake HID, real feedback code)."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS and Xcode command line tools')
class JoyConLeftTests(unittest.TestCase):
    def test_device_lifetime_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'lifetime'
            build = subprocess.run([
                'xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', f'-I{TOTK}/runtime',
                str(TOTK / 'tests/joycon_left/lifetime.c'), '-o', str(probe),
            ], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(probe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('369600 lifecycle schedules', run.stdout)

    def test_captured_report_decoding(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'decode'
            build = subprocess.run([
                'xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{TOTK}/runtime',
                str(TOTK / 'tests/joycon_left/decode.c'), '-o', str(probe),
            ], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(probe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS:', run.stdout)

    def test_input_ownership_and_lifetime(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'probe'
            build = subprocess.run([
                'xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                str(TOTK / 'tests/joycon_left/probe.m'),
                '-framework', 'Foundation', '-framework', 'GameController', '-framework', 'IOKit',
                '-framework', 'CoreHaptics', '-framework', 'AppKit', '-o', str(probe),
            ], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for case in ('gyro-off', 'native-motion', 'motion-stop', 'motion-switch', 'unrelated-detach',
                         'reconnect', 'rumble-order', 'rumble-stop', 'late-open', 'pro-controller', 'pro-alongside',
                         'simple-mode', 'mode-retry', 'mode-cancel', 'mode-already-full',
                         'right-simple-mode', 'right-mode-retry', 'right-mode-cancel', 'right-mode-already-full',
                         'both-simple-modes', 'joycon-before-pro', 'slow-mode-write', 'both-slow-mode-writes',
                         'mode-write-timeout', 'simple-stick-blocked-mode',
                         'rapid-physical-reconnect-pending-output', 'rumble-reconnect-pending-output',
                         'owner-replaced-before-detach', 'retired-report-callback',
                         'recovery-budget-after-success', 'recovery-while-write-pending',
                         'overlapping-retirements', 'many-reused-retirements', 'generic-shared-lifetime',
                         'generic-imu-regroup', 'generic-imu-rejected', 'generic-pending-attach',
                         'switch-rumble-independent-binding', 'switch-rumble-restart',
                         'generic-stale-sensor', 'managed-open-failure', 'managed-exclusive-open',
                         'motion-init-owner-changed', 'motion-init-restarted'):
                with self.subTest(case=case):
                    run = subprocess.run([str(probe), case], capture_output=True, text=True, timeout=15)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn('PASS:', run.stdout)
