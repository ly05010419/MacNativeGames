"""Exercise real Metal upload ownership at a dependent-allocation boundary."""
import json
import os
import subprocess
import tempfile
from pathlib import Path
import unittest


@unittest.skipUnless(all(os.environ.get(k) for k in
                         ('MNG_OOT_PROBE', 'MNG_OOT_GAME_DATA', 'MNG_OOT_REWRITTEN')),
                     'requires the built OOT Metal runtime and verified local data')
class UploadTransactionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.trace = Path(self.tmp.name) / 'batches.tsv'

    def run_probe(self, *, pair=False, negative=False):
        env = dict(os.environ, TOTK_UPLOAD_MEMORY_FILE=str(self.trace),
                   TOTK_GAME_DATA=os.environ['MNG_OOT_GAME_DATA'],
                   TOTK_SLOT_OWNER='codex', TOTK_SLOT_PURPOSE='upload-transaction-regression',
                   TOTK_SLOT_EXPECT_S='30')
        for name in ('TOTK_PROBE_UPLOAD_PAIR', 'TOTK_PROBE_UPLOAD_SPLIT', 'TOTK_PROBE_NEGATIVE'):
            env.pop(name, None)
        if pair:
            env['TOTK_PROBE_UPLOAD_PAIR'] = '1'
        if negative:
            env['TOTK_PROBE_UPLOAD_SPLIT'] = '1'
        result = subprocess.run([os.environ['MNG_OOT_PROBE'], os.environ['MNG_OOT_REWRITTEN'],
                                 'probe-gpu-upload-batch'], env=env, text=True, capture_output=True, timeout=90)
        return result, result.stdout + result.stderr

    def check_pass(self, result, log, count):
        self.assertEqual(result.returncode, 0, log[-10000:])
        records = [json.loads(line) for line in log.splitlines() if line.startswith('{"status":')]
        self.assertTrue(records, log)
        self.assertEqual(records[-1]['status'], 'pass')
        self.assertEqual(records[-1]['probe'], 'probe-gpu-upload-batch')
        self.assertEqual(records[-1]['checked'], count)
        rows = [line.split('\t') for line in self.trace.read_text().splitlines()]
        submits = {r[1]: r for r in rows if r[0] == 'submit'}
        completed = {r[1]: r for r in rows if r[0] == 'complete'}
        self.assertTrue(submits)
        self.assertEqual(set(submits), set(completed))
        for seq, row in submits.items():
            self.assertEqual(row[1:-1], completed[seq][1:-1])
            self.assertEqual(completed[seq][-1], '0', completed[seq])
            self.assertGreaterEqual(int(row[3]), int(row[5]) + int(row[6]))

    def test_dependent_scratch_stays_in_one_batch(self):
        result, log = self.run_probe(pair=True)
        self.check_pass(result, log, 258)

    def test_old_split_retires_first_scratch_too_early(self):
        result, log = self.run_probe(pair=True, negative=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('FATAL upload pair probe: first scratch belongs to a retired batch', log)

    def test_staging_bound_retirement_and_pixels(self):
        result, log = self.run_probe()
        self.check_pass(result, log, 61)


if __name__ == '__main__':
    unittest.main()
