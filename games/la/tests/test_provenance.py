"""Every file of this folder is accounted for (third_party/la-import.json): copied unchanged, copied and changed, or new, and the size rule for each.
Files of more than 300 lines exist only as unchanged copies, except those the owner allowed to change, each with its own growth limit (2026-11-07: the importer and the
rewriter by five lines; hle_movie.c by 55 for nxnative's native-texture slot fix), and the new warm-up seed, generated data, up to 1,500 lines (2026-11-08)."""
import hashlib
import json
from pathlib import Path
import unittest

GAME = Path(__file__).resolve().parents[1]
RECORD = GAME/'third_party/la-import.json'
LARGE_CHANGED = {'third_party/nxnative/build_ipa_from_nsp.py': 5, 'runtime/tools/patch_nso.py': 5, 'runtime/host/hle_movie.c': 55,
                 'runtime/host/front_mac.m': 0,   # front_mac.m: the saved FPS target no longer loaded or saved (Albert, 2026-11-08: three lines, no growth)
                 'runtime/host/hle_hid.c': 13,    # DM_HID_LOG=2 (the Marin investigation) and scripted presses that wait for a released sample (Albert, 2026-11-08: 13 lines accepted, the shared press kept readable)
                 'runtime/gfx/nvn/nvn_mtl.m': 0}  # pso_user_path no longer static, for the shader list export (Albert, 2026-11-08: that change only, no growth)
NEW_LARGE = {'runtime/games/01006BB00C6F0000/pso_manifest.tsv': 1500}   # the bundled warm-up seed, generated data (Albert, 2026-11-08: up to 1,500 lines)
LIMIT = 300


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
        intact, modified = self.record['intact'], self.record['modified']
        for rel, b in files():
            sha, lines = hashlib.sha256(b).hexdigest(), b.count(b'\n')
            with self.subTest(rel):
                if rel in intact:
                    self.assertEqual(sha, intact[rel], 'changed since the copy: record it as modified')
                elif rel in modified:
                    m = modified[rel]
                    self.assertEqual(sha, m['after'], 'changed since the record: regenerate it')
                    self.assertEqual(lines, m['after_lines'])
                    if m['before_lines'] > LIMIT:
                        self.assertIn(rel, LARGE_CHANGED, 'a large copied file may change only with the owner\'s agreement')
                        self.assertLessEqual(lines, m['before_lines']+LARGE_CHANGED[rel])
                    else:
                        self.assertLessEqual(lines, LIMIT)
                else:
                    self.assertLessEqual(lines, NEW_LARGE.get(rel, LIMIT), 'a new file keeps to 300 lines')

    def test_left_out_files_stay_out(self):
        for rel in self.record['left_out']:
            self.assertFalse((GAME/rel).exists(), rel)

    def test_recorded_files_exist(self):
        present = {rel for rel, _ in files()}
        self.assertFalse((set(self.record['intact']) | set(self.record['modified'])) - present)


if __name__ == '__main__':
    unittest.main()
