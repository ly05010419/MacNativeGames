"""Every file of this folder is accounted for (third_party/ss-import.json): copied unchanged from its origin, copied and changed, or new, with the size rule for each.
Origins: LA's target layer (games/la, MNG 549b8a9a), nxnative b1dee89's dm/ (the runtime, the translator and SPIRV-Cross binaries, the NSP script) and MNG's own runtime files as LA
has them. Files of more than 300 lines exist only as unchanged copies, except those the owner allowed to change, each with its own growth limit (AGREED). A large file changed
here but not yet agreed (PENDING) is reported as skipped, never as passing. New files keep to 300 lines."""
import hashlib
import json
from pathlib import Path
import unittest

GAME = Path(__file__).resolve().parents[1]
RECORD = GAME/'third_party/ss-import.json'
AGREED = {   # Albert, 2026-11-11 ("yo alberto acepto los 12 archivos"): each changed large file, its growth over its origin as the limit
    'runtime/gfx/nvn/hle_nvn.c': 23,   # 2854 -> 2877 lines over nxnative b1dee89 dm/gfx/nvn/hle_nvn.c: MNG: AoC timing hooks kept inert, corner-box statistics, quieter logs (weak g_flag for unit tests), DM_LONGFRAME opt-in
    'runtime/gfx/nvn/nvn_mtl.m': 38,   # 12469 -> 12507 lines over nxnative b1dee89 dm/gfx/nvn/nvn_mtl.m: MNG: feedback-snap flag (afr), statistics, non-static pso_user_path; DLSS offsets behind the macOS 27 SDK; nxnative's link-hold threshold kept, with a note
    'runtime/gfx/tools/dmsh_build.py': 12,   # 1396 -> 1408 lines over nxnative b1dee89 dm/gfx/tools/dmsh_build.py: MNG: offline Metal only, the 4-column shader map reader
    'runtime/gfx/tools/shader_split.py': 0,   # 2811 -> 2810 lines over nxnative b1dee89 dm/gfx/tools/shader_split.py: MNG: map written without source paths
    'runtime/host/front_mac.m': 243,   # 1363 -> 1606 lines over nxnative b1dee89 dm/host/front_mac.m: MNG: settings window, corner box, shader list export, face-button layouts
    'runtime/host/hle_hid.c': 18,   # 894 -> 912 lines over nxnative b1dee89 dm/host/hle_hid.c: MNG: DM_CTL fps lines, DM_HID_LOG=2
    'runtime/host/hle_movie.c': 2,   # 875 -> 877 lines over nxnative b1dee89 dm/host/hle_movie.c: MNG: two lines of the native-texture slot handling
    'runtime/host/loader.c': 0,   # 724 -> 724 lines over nxnative b1dee89 dm/host/loader.c: MNG: AoC timing ABI and state page (weak aoc_timing_apply for host unit tests)
    'runtime/host/tests/t_hid.c': 5,   # 500 -> 505 lines over nxnative b1dee89 dm/host/tests/t_hid.c: MNG: AoC timing stubs
    'runtime/tools/nx_extract/Program.cs': 0,   # 395 -> 241 lines over nxnative b1dee89 dm/tools/nx_extract/Program.cs: LA: the verified extractor (Select.cs) in place of nxnative's
    'runtime/tools/patch_nso.py': 0,   # 1751 -> 1751 lines over nxnative b1dee89 dm/tools/patch_nso.py: MNG: AoC timing ABI line, 0x90 state page
    'third_party/nxnative/build_ipa_from_nsp.py': 71,   # 2734 -> 2805 lines over nxnative b1dee89 scripts/build_ipa_from_nsp.py: MNG: extracted mode (this target), rewrite proof, 4-column map, offline nx_extract
}
PENDING = {}   # large files changed but not yet agreed (reported as skipped)
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
                    if m['before_lines'] > LIMIT:
                        if rel in AGREED: self.assertLessEqual(lines, m['before_lines'] + AGREED[rel])
                        elif rel in PENDING: self.skipTest(f'large file changed, awaiting the owner\'s agreement: {PENDING[rel]}')
                        else: self.fail('a large copied file may change only with the owner\'s agreement')
                    else:
                        self.assertLessEqual(lines, LIMIT)
                else:
                    self.assertIn(rel, new, 'not in the record')
                    self.assertLessEqual(lines, LIMIT, 'a new file keeps to 300 lines')

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
