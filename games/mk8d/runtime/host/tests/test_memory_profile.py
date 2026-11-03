"""The NSP heap probe must still understand real dispatcher diagnostics."""
import os
import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'games'))
from profile_guess import heap_signals


class MemoryProfileTests(unittest.TestCase):
    def test_dispatcher_logs(self):
        result = subprocess.run([str(HERE / os.environ.get('DM_TEST_BUILD', 'build') / 't_svc_memory'), '--heap-probe'],
                                capture_output=True, text=True, check=True)
        signals = heap_signals(result.stderr, {
            'mem_reserve': '0x140000000', 'mem_stack': '0x10000000',
            'mem_alias': '0xb8000000', 'mem_used': '0x200000',
            'app_pool': '0x1000000', 'sys_resource': '0x2000000',
        })
        self.assertEqual(signals['mem_alias'][0], '0x118000000')
        self.assertEqual(signals['mem_used'][0], '0xe00000')


if __name__ == '__main__':
    unittest.main()
