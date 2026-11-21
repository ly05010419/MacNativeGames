"""shader_split: a file no reader can read is left out with a red line, and the split goes on (PR #282: one Next Level Games archive of a revision nlg_dict does not
know ended a whole split with nothing written). nlg_dict checks its layout before reading it, so such an index is named, not an IndexError. No game files needed."""
import contextlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '01007BB017812000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def index(nchunk, nrec, recs=None, chunks=None, names=b'.data\0'):   # nlg_dict's layout: header, 0x18-byte records, 0x10-byte chunks, names
    d = ss.NLG_DICT + bytes([4, 1, 1, 0]) + struct.pack('<I', 0x1000) + bytes([nchunk, nrec, names.count(b'\0'), 0])
    for r in (recs if recs is not None else [bytes(16)] * nrec): d += struct.pack('<IHH', 0, 0, 0) + r
    for c in (chunks if chunks is not None else [(0, 0, 0, 0)] * nchunk): d += struct.pack('<3IHBB', *c[:3], 0, c[3], 0)
    return d + names


class NlgLayout(unittest.TestCase):
    def test_known_layout_reads(self):
        self.assertEqual(ss.nlg_dict(index(2, 1, [bytes([0, 1]) + bytes(14)]), '/a.dict'), (1, [(0, [0, 1] + [0] * 14)], [(0, 0, 0, 0), (0, 0, 0, 0)], ['.data']))

    def test_other_layouts_are_named(self):
        for name, d in (('cut short', index(5, 2)[:0x28]), ('header only', ss.NLG_DICT + bytes(4)),
                        ('section chunk out of range', index(2, 1, [bytes([0, 2]) + bytes(14)])), ('more records than chunks', index(1, 2))):
            with self.subTest(name), self.assertRaisesRegex(SystemExit, r"^\[RED\] /a\.dict: the index .* is not in the layout of Luigi's Mansion 3's archives"):
                ss.nlg_dict(d, '/a.dict')


class ScanFile(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(lambda: __import__('shutil').rmtree(self.tmp))

    def scan(self, path, rel):
        out = io.StringIO()
        with contextlib.redirect_stdout(out): r = ss.scan_file((str(path), rel))
        return r, out.getvalue()

    def test_red_line_of_a_reader(self):
        p = self.tmp / 'a.dict'; p.write_bytes(index(5, 2)[:0x28])
        (rel, keys, pairs, extra), out = self.scan(p, 'sub/a.dict')
        self.assertEqual((rel, keys, pairs, extra[0]), ('sub/a.dict', {}, set(), 'unreadable'))
        self.assertTrue(extra[1].startswith('/sub/a.dict: the index'))
        self.assertEqual(out.count('/sub/a.dict'), 1); self.assertTrue(out.startswith('[RED] /sub/a.dict: the index') and out.endswith('(its shaders are left out)\n'))

    def test_exception_of_a_reader(self):
        p = self.tmp / 'a.dict'; p.write_bytes(index(1, 1))
        with mock.patch.object(ss, 'nlg_shaders', side_effect=IndexError('list index out of range')):
            (_, _, _, extra), out = self.scan(p, 'a.dict')
        self.assertEqual(extra, ('unreadable', 'a.dict: IndexError: list index out of range'))
        self.assertEqual(out, '[RED] a.dict: IndexError: list index out of range (its shaders are left out)\n')

    def test_interrupt_is_not_taken(self):
        p = self.tmp / 'a.dict'; p.write_bytes(index(1, 1))
        with mock.patch.object(ss, 'nlg_shaders', side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt): self.scan(p, 'a.dict')

    def test_split_goes_on(self):   # the whole split: the unreadable archive is reported once more at the end, the tables are written and the split exits 0
        rom = self.tmp / 'RomFS'; (rom / 'sub').mkdir(parents=True)
        (rom / 'sub/a.dict').write_bytes(index(5, 2)[:0x28]); (rom / 'sub/a.data').write_bytes(bytes(64)); (rom / 'b.bin').write_bytes(b'no shaders')
        main = self.tmp / 'main'; main.write_bytes(b'NSO0' + bytes(0xfc))   # an NSO header of three empty segments
        out = [self.tmp / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
        r = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main, *out], capture_output=True, text=True,
                           env=dict(os.environ, DM_GAME='01007BB017812000', J='2'))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('\n[RED] 1 file(s) could not be read, their shaders are left out (first: /sub/a.dict: the index', '\n' + r.stdout)
        self.assertIn('shaders: 0 (', r.stdout)
        for p in out: self.assertTrue(p.exists(), p)


if __name__ == '__main__': unittest.main()
