#!/usr/bin/env python3
"""CPU-only production-header regression fixtures; no Metal, game data or GPU."""
import csv
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
INCLUDE = HERE.parent / 'games/totk/third_party/d2r_gfx/gfx/nvn'


class PresentationEvents(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='oot-telemetry-fixture-')
        cls.folder = Path(cls.temporary.name)
        cls.executable = cls.folder / 'present-events'
        subprocess.run(['/usr/bin/cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-I', str(INCLUDE), str(HERE / 'nvn_present_events_probe.c'), '-lm',
                        '-o', str(cls.executable)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def invoke(self, mode, enabled):
        environment = {k: v for k, v in os.environ.items() if k != 'DM_NVN_TSV'}
        path = self.folder / mode
        if enabled:
            environment['DM_NVN_TSV'] = str(path)
        subprocess.run([str(self.executable), mode], env=environment, check=True, timeout=15)
        return Path(str(path) + '.presented.tsv')

    def test_disabled_creates_no_file(self):
        self.assertFalse(self.invoke('disabled', False).exists())

    def test_late_wrapped_concurrent_events_and_invalid_gpu_times(self):
        path = self.invoke('enabled', True)
        with path.open() as stream:
            self.assertIn('clock=mach_absolute_time_seconds', next(stream))
            rows = list(csv.DictReader(stream, delimiter='\t'))
        self.assertEqual(len(rows), 533)
        for row in rows:
            self.assertEqual(len(row), 8)
            self.assertTrue(math.isfinite(float(row['presented_s'])))
            self.assertGreater(float(row['observed_s']), 0)
        terminals = {(int(r['ticket']), int(r['generated'])): r for r in rows if r['event'] != 'queued'}
        self.assertEqual(terminals[(1, 0)]['frame'], '10')
        self.assertEqual(terminals[(1, 0)]['serial'], '7001')
        self.assertEqual(float(terminals[(1, 0)]['presented_s']), 1000.01)
        self.assertGreater(float(terminals[(1, 0)]['observed_s']), 2000)
        self.assertEqual(terminals[(1025, 0)]['frame'], '1034')
        self.assertEqual(terminals[(3, 0)]['event'], 'dropped')
        self.assertEqual(terminals[(3, 1)]['event'], 'presented')
        self.assertEqual(terminals[(4, 0)]['reason'], 'drawable_not_presented')
        self.assertEqual(terminals[(5, 0)]['event'], 'invalid')
        self.assertEqual(terminals[(6, 0)]['event'], 'invalid')
        self.assertNotIn((7, 0), terminals)
        self.assertEqual(sum(r['ticket'] == '7' and r['event'] == 'queued' for r in rows), 1)
        self.assertEqual(terminals[(0, 0)]['reason'], 'discard_before_enqueue')
        self.assertEqual(terminals[(8, 0)]['presented_s'], terminals[(9, 0)]['presented_s'])
        for identity in range(256):
            ticket = 10000 + identity
            matching = [r for r in rows if int(r['ticket']) == ticket]
            self.assertEqual(len(matching), 2)
            self.assertEqual({r['event'] for r in matching}, {'queued', 'presented'})
            self.assertEqual({int(r['frame']) for r in matching}, {20000 + identity})
            self.assertEqual({int(r['serial']) for r in matching}, {30000 + identity})


if __name__ == '__main__':
    unittest.main()
