"""The pairing panel must keep main-queue hotplug callbacks runnable while waiting."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'macOS run-loop regression')
class ControllerRunloop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='lm3-controller-runloop-')
        cls.executable = Path(cls.work.name) / 'probe'
        subprocess.run([
            'xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT / 'runtime'), str(ROOT / 'tests/controller_runloop_probe.m'),
            '-framework', 'Foundation', '-framework', 'AppKit', '-framework', 'CoreFoundation',
            '-o', str(cls.executable),
        ], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_probe(self, *arguments):
        # The harness bounds its run loops; allow extra process-start time on
        # a cold or busy Mac without changing the callback-delivery oracle.
        result = subprocess.run([str(self.executable), *arguments], capture_output=True,
                                text=True, timeout=5)
        return result, json.loads(result.stdout)

    def test_actual_helper_services_main_queue_inside_modal_mode(self):
        result, data = self.run_probe()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(data['status'], 'pass')
        self.assertEqual((data['nested_gcd'], data['nested_operation'], data['nested_timer']), (1, 1, 1))
        self.assertEqual((data['wait_completed'], data['on_main']), (1, 1))

    def test_original_dispatch_sync_strategy_fails_same_oracle(self):
        result, data = self.run_probe('old-dispatch-sync')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('modal-loop main-queue delivery failed', result.stderr)
        self.assertEqual((data['nested_gcd'], data['nested_operation'], data['nested_timer']), (0, 0, 1))

    def test_main_thread_fast_path_remains_synchronous(self):
        result, data = self.run_probe('main')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(data, {'main_fast_path': 1, 'on_main': 1})


if __name__ == '__main__':
    unittest.main()
