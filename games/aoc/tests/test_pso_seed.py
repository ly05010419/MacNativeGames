"""scripts/pso_seed.py: rows merged file by file in the order given, every malformed row refused with its line, the seed never cut;
and the bundled seed itself (runtime/games/<title>/pso_manifest.tsv): valid rows, no repeats, its sources named, within its limit."""
import hashlib
import struct
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
        self.assertEqual(self.L['size'] * 2, 864)   # the length of every R row in Age of Calamity's recordings of 2026-11-08
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

    def test_rows_differing_only_in_unused_colour_state_are_repeats(self):
        L = self.L; row = bytearray.fromhex(self.r_row()[2:])
        struct.pack_into('<8I', row, L['cfmt'], 80, 0, 0, 0, 0, 0, 0, 0); struct.pack_into('<I', row, L['blend_en'], 0)
        other = bytearray(row); other[L['blend'] + 1] ^= 0xff   # attachment 0's factors, blending off
        third = bytearray(row); struct.pack_into('<I', third, L['chmask'], struct.unpack_from('<I', row, L['chmask'])[0] ^ 0xf0)   # attachment 1 absent
        kept = bytearray(row); struct.pack_into('<I', kept, L['chmask'], struct.unpack_from('<I', row, L['chmask'])[0] ^ 0x0f)   # attachment 0 present: kept
        with tempfile.TemporaryDirectory() as tmp:
            rows = pso_seed.merge(self.files(tmp, ''.join('R\t' + r.hex() + '\n' for r in (row, other, third, kept))), L)
        self.assertEqual(rows, ['R\t' + row.hex(), 'R\t' + kept.hex()])

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
        seed = GAME / 'runtime/games/01002B00111A2000/pso_manifest.tsv'
        text = seed.read_text(); lines = text.splitlines()
        self.assertTrue(text.startswith(pso_seed.HEADER)); self.assertLessEqual(len(lines), 1500)   # the limit agreed for Link's Awakening, proposed here (2026-11-08)
        named = [x for x in lines if x.startswith('# source ')]
        self.assertTrue(named and all(x.split()[2] == str(i) and len(x.split()[-1]) == 64 for i, x in enumerate(named, 1)))
        rows = [x for x in lines if not x.startswith('#')]
        self.assertEqual(pso_seed.merge([seed], self.L), rows)   # every row valid, none repeated, in file order

    def test_shader_map_refuses_unknown_keys(self):
        good, other = self.r_row(), self.r_row(fs=b'00000000000000cc_b')
        with tempfile.TemporaryDirectory() as tmp:
            smap = Path(tmp) / 'shader_map.tsv'
            smap.write_text('# kind: shader map\na.bin\t0\t0\t00000000000000aa\nb.bin\t0\t0\t00000000000000bb\nc.txt\t0\t0\t00000000000000cc\n')
            known = pso_seed.shader_map_keys(smap)
            self.assertEqual(known, {'00000000000000aa', '00000000000000bb'})   # only .bin rows are translated shaders
            (path,) = self.files(tmp, f'S\t00000000000000aa_b\t0\n{good}\nC\t00000000000000bb_b\n')
            self.assertEqual(len(pso_seed.merge([path], self.L, known)), 3)
            for name, row in {'S key': 'S\t00000000000000dd_b\t0', 'C key': 'C\t00000000000000dd_b', 'R fragment key': other,
                              'R vertex key': self.r_row(vs=b'00000000000000dd_b')}.items():
                with self.subTest(name):
                    (path,) = self.files(tmp, f'{good}\n{row}\n')
                    with self.assertRaisesRegex(pso_seed.SeedError, r'rec0\.tsv:2: shader key not in the shader map'):
                        pso_seed.merge([path], self.L, known)
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
