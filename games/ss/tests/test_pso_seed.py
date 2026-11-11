"""scripts/pso_seed.py: rows merged file by file in the order given, every malformed row refused with its line, the seed never cut;
and the bundled seed itself (runtime/games/<title>/pso_manifest.tsv): valid rows, no repeats, its sources named, within its limit."""
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME / 'scripts'))
import pso_seed  # noqa: E402


@unittest.skipUnless(sys.platform == 'darwin', 'the key layout is read with clang from the host header')
class PsoSeedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.L = pso_seed.layout()

    def r_row(self, vs=b'00000000000000aa_b', fs=b'00000000000000bb_b', stages=(0, 1), merged=0, fz=0, tail=7):
        L = self.L; b = bytearray((i * tail) & 0xff for i in range(L['size']))
        b[L['vs']:L['vs'] + L['keylen']] = vs.ljust(L['keylen'], b'\0'); b[L['fs']:L['fs'] + L['keylen']] = fs.ljust(L['keylen'], b'\0')
        b[L['vs_stage']], b[L['fs_stage']], b[L['merged']], b[L['fz']] = stages[0], stages[1], merged, fz
        return 'R\t' + b.hex()

    def files(self, tmp, *texts):
        paths = []
        for i, text in enumerate(texts):
            p = Path(tmp) / f'rec{i}.tsv'; p.write_text(text); paths.append(p)
        return paths

    def test_layout_is_the_hosts_key(self):
        self.assertEqual(self.L['size'] * 2, 880)   # the length of every R row recorded on Skyward Sword HD 2026-11-11 (validation/run1: 153 rows; nxnative b1dee89's key is 8 bytes longer than LA's)
        self.assertEqual((self.L['vs'], self.L['fs'], self.L['keylen']), (0, 24, 24))

    def test_file_order_and_repeats(self):
        r1, r2 = self.r_row(tail=3), self.r_row(tail=5)
        a = f'S\tffffffffffffffff_b\t0\n{r1}\nC\t0000000000000001_b\n'
        b = f'{r1}\nS\t0000000000000002_b\t1\n# a comment\n{r2}\nS\tffffffffffffffff_b\t0\n'
        with tempfile.TemporaryDirectory() as tmp:
            rows = pso_seed.merge(self.files(tmp, a, b), self.L)
        want = ['S\tffffffffffffffff_b\t0', r1, 'C\t0000000000000001_b', 'S\t0000000000000002_b\t1', r2]
        self.assertEqual(rows, want)
        self.assertNotEqual(sorted(want), want)   # control: sorting would change the order the host pre-builds in

    def test_malformed_rows_are_refused_with_their_line(self):
        good = self.r_row()
        bad = {'R 862 digits': good[:-2], 'R 866 digits': good + '00', 'uppercase hex': good.upper().replace('R\t', 'R\t'),
               'unknown kind': 'X\t0000000000000001_b', 'extra column': 'C\t0000000000000001_b\t5', 'stage 6': 'S\t0000000000000001_b\t6',
               'short key': 'S\t000000000000001_b\t0', 'empty line': '', 'vertex key empty': self.r_row(vs=b''),
               'bytes after the key': self.r_row(fs=b'00000000000000bb_b\0x'), 'fz 2': self.r_row(fz=2), 'R stage 6': self.r_row(stages=(0, 6))}
        with tempfile.TemporaryDirectory() as tmp:
            for name, row in bad.items():
                with self.subTest(name):
                    (path,) = self.files(tmp, f'C\t0000000000000009_b\n{row}\n{good}\n')
                    with self.assertRaisesRegex(pso_seed.SeedError, rf'rec0\.tsv:2: '):
                        pso_seed.merge([path], self.L)
            (path,) = self.files(tmp, f'{good}\n{self.r_row(fs=b"")}\n')   # control: a pipeline with no fragment shader is a good row
            self.assertEqual(len(pso_seed.merge([path], self.L)), 2)

    def test_bundled_seed(self):
        seed = GAME / 'runtime/games/01002DA013484000/pso_manifest.tsv'
        text = seed.read_text(); lines = text.splitlines()
        if not text.startswith(pso_seed.HEADER):   # no list recorded yet: nxnative's provisional one, empty and as imported (third_party/ss-profile-import.json)
            import hashlib
            row = next(r for r in json.loads((GAME / 'third_party/ss-profile-import.json').read_text()) if r['destination'].endswith('pso_manifest.tsv'))
            self.assertEqual(hashlib.sha256(seed.read_bytes()).hexdigest(), row['sha256'])
            self.assertEqual([x for x in lines if x.strip() and not x.startswith('#')], [])
            return
        self.assertLessEqual(len(lines), 1500)   # the size agreed for LA's seed (Albert, 2026-11-08); Skyward Sword HD's list needs its own agreement
        named = [x for x in lines if x.startswith('# source ')]
        self.assertTrue(named and all(x.split()[2] == str(i) and len(x.split()[-1]) == 64 for i, x in enumerate(named, 1)))
        rows = [x for x in lines if not x.startswith('#')]
        self.assertEqual(pso_seed.merge([seed], self.L), rows)   # every row valid, none repeated, in file order

    def test_export_header_matches_the_key_layout(self):
        text = (GAME / 'runtime/host/pso_export.h').read_text()   # the settings window's export checks rows with these numbers
        got = {k: int(v) for k, v in re.findall(r'DM_PSO_(\w+) = (\d+)', text)}
        want = {'SKEY_SIZE': self.L['size'], 'VS': self.L['vs'], 'FS': self.L['fs'], 'KEYLEN': self.L['keylen'], 'VS_STAGE': self.L['vs_stage'],
                'FS_STAGE': self.L['fs_stage'], 'MERGED': self.L['merged'], 'FZ': self.L['fz']}
        self.assertEqual(got, want)

    def test_shader_map_refuses_unknown_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            m = Path(tmp) / 'shader_map.tsv'; m.write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\na.bin\t0\tvertex\t00000000000000aa\na.bin\t1\tpixel\t00000000000000bb\n')
            keys = pso_seed.map_keys(m); self.assertEqual(keys, {'00000000000000aa_b', '00000000000000bb_b'})
            good = f'# exported\nS\t00000000000000aa_b\t0\n{self.r_row()}\n{self.r_row(fs=b"")}\n'
            (path,) = self.files(tmp, good)
            self.assertEqual(len(pso_seed.merge([path], self.L, keys)), 3)   # control: every key in the map
            for name, row in {'S': 'S\t00000000000000cc_b\t0', 'C': 'C\t00000000000000cc_b', 'R fragment': self.r_row(fs=b'00000000000000cc_b')}.items():
                with self.subTest(name):
                    (path,) = self.files(tmp, f'S\t00000000000000aa_b\t0\n{row}\n')
                    with self.assertRaisesRegex(pso_seed.SeedError, r'rec0\.tsv:2: shader key not in the shader map: 00000000000000cc_b'):
                        pso_seed.merge([path], self.L, keys)
                    self.assertEqual(len(pso_seed.merge([path], self.L)), 2)   # control: without the map the row is well formed

    def run_cli(self, tmp, *args):
        return subprocess.run([sys.executable, str(GAME / 'scripts/pso_seed.py'), *args], capture_output=True, text=True, cwd=tmp)

    def test_written_seed_and_the_line_limit(self):
        rows = [f'S\t{i:016x}_b\t0' for i in range(5)]   # with the header's two lines and one source line: 8
        with tempfile.TemporaryDirectory() as tmp:
            (rec,) = self.files(tmp, '\n'.join(rows) + '\n')
            out = Path(tmp) / 'pso_manifest.tsv'
            r = self.run_cli(tmp, '--output', str(out), '--max-lines', '7', str(rec))
            self.assertNotEqual(r.returncode, 0); self.assertIn('8 lines, more than 7', r.stderr); self.assertFalse(out.exists())
            r = self.run_cli(tmp, '--output', str(out), '--max-lines', '8', str(rec))
            self.assertEqual(r.returncode, 0, r.stderr)
            src = f'# source 1 {Path(tmp).name}/rec0.tsv sha256 {hashlib.sha256(rec.read_bytes()).hexdigest()}\n'
            self.assertEqual(out.read_text(), pso_seed.HEADER + src + ''.join(x + '\n' for x in rows))
            r = self.run_cli(tmp, '--output', str(out), str(rec))
            self.assertNotEqual(r.returncode, 0); self.assertIn('exists', r.stderr)


if __name__ == '__main__':
    unittest.main()
