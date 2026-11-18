"""Keep the reviewed inventory and digests in step with the TMNT source tree."""
import hashlib
import json
import subprocess
import unittest
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]
MANIFEST = 'third_party/tmnt-import.json'


class ProvenanceTests(unittest.TestCase):
    def test_inventory_and_copies(self):
        record = json.loads((GAME / MANIFEST).read_text())
        copied = record['files']
        local = set(record['local_files'])
        tracked = set(subprocess.check_output(
            ['git', 'ls-files', '-z', '--', '.'], cwd=GAME).decode().strip('\0').split('\0'))
        self.assertFalse(local.intersection(copied))
        self.assertEqual(set(copied) | local | {MANIFEST}, tracked)
        for name, source in copied.items():
            with self.subTest(file=name):
                self.assertRegex(source['commit'], r'^[a-f0-9]{40}$')
                self.assertRegex(source['source_blob'], r'^[a-f0-9]{40}$')
                self.assertTrue(source['origin'])
                self.assertEqual(hashlib.sha256((GAME / name).read_bytes()).hexdigest(),
                                 source['sha256'])
