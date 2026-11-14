"""Every shipped source has a recorded origin and current hash.

The contributor's private file-size approval policy is not an MNG policy. Preserve
source provenance without depending on approvals in another repository's chat.
"""
import hashlib
import json
from pathlib import Path
import unittest

GAME = Path(__file__).resolve().parents[1]
RECORD = GAME/'third_party/ss-import.json'


def files():
    for p in sorted(GAME.rglob('*')):
        rel = p.relative_to(GAME).as_posix()
        if p.is_file() and '__pycache__' not in p.parts and p.name != '.DS_Store' and p != RECORD and not any(part in ('bin', 'obj') for part in p.relative_to(GAME).parts):
            yield rel, p.read_bytes()


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.record = json.loads(RECORD.read_text())

    def test_record_is_one_line(self):
        self.assertEqual(RECORD.read_text().count('\n'), 1)

    def test_every_file_is_accounted_for(self):
        intact, modified, new = self.record['intact'], self.record['modified'], set(self.record['new'])
        for rel, b in files():
            sha, lines = hashlib.sha256(b).hexdigest(), b.count(b'\n')
            with self.subTest(rel):
                if rel in intact:
                    self.assertEqual(sha, intact[rel], 'changed since the copy: record it as modified')
                elif rel in modified:
                    m = modified[rel]
                    self.assertEqual(sha, m['after'], 'changed since the record: regenerate it')
                    self.assertEqual(lines, m['after_lines'])
                else:
                    self.assertIn(rel, new, 'not in the record')

    def test_every_copy_names_its_origin(self):
        self.assertEqual(set(self.record['origins']), set(self.record['intact']) | set(self.record['modified']))

    def test_left_out_files_stay_out(self):
        for rel in self.record['left_out']:
            self.assertFalse((GAME/rel).exists(), rel)

    def test_recorded_files_exist(self):
        present = {rel for rel, _ in files()}
        self.assertFalse((set(self.record['intact']) | set(self.record['modified']) | set(self.record['new'])) - present)


if __name__ == '__main__':
    unittest.main()
