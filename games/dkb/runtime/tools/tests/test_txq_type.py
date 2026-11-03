"""A size query takes its texture's type from the shader's other instructions on that texture (dmsh_xlate Accessor.TextureTypesOf), using synthetic shaders only.

The handle of a bindless TXQ and of a TEX OR the same texture word in c[2] with different sampler words, so they are two bindings of one texture slot, as in Princess Peach: Showtime!.
Needs the translator:
  DMSH_TEST_XLATE=$PWD/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate python3 -m unittest discover -s dm/tools/tests -p test_txq_type.py -v
No game inputs are used.
"""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TABLE = (ROOT / 'dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs').read_text()
TEXTURE_2D, TEXTURE_3D, TEXTURE_CUBE, ARRAY = 3, 4, 5, 1 << 8   # SamplerType
DIM_2D, DIM_2D_ARRAY, DIM_3D, DIM_CUBE = 2, 3, 4, 6             # TexDim
SLOT_128, SLOT_129 = 0x168, 0x170                               # c[2] offsets of separate textures
SAMPLER_1, SAMPLER_6 = 0x570, 0x598                             # and of separate samplers


def base(name):
    match = re.search(r'Add\("([01x]{64})", InstName\.' + name + r'\s*,', TABLE)
    assert match, f'{name} is not in InstTable.cs'
    return int(match[1].replace('x', '0'), 2)


def ldc(rd, offset):                                            # LDC.32 Rd, c[2][offset], RZ
    return base('Ldc') | (4 << 48) | (2 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | rd


def handle(rd, texture, sampler):                               # Rd = c[2][texture] | c[2][sampler], and R10 + R11 as scratch
    return [ldc(10, texture), ldc(11, sampler), base('Lop') | (1 << 41) | (11 << 20) | (7 << 16) | (10 << 8) | rd]


def mov(rd, value):
    return base('Mov32i') | (value << 20) | (7 << 16) | (15 << 12) | rd


def query(rd, texture, sampler=SAMPLER_6):                      # TXQ.B Rd, width, handle in R4 and level in R5
    return handle(4, texture, sampler) + [mov(5, 0), base('TxqB') | (1 << 31) | (1 << 22) | (7 << 16) | (4 << 8) | rd]


def sample(rd, texture, dim, sampler=SAMPLER_1):                # TEX.B.LZ Rd, red, coordinates in R0.., handle in R6
    return handle(6, texture, sampler) + [base('TexB') | (1 << 37) | (1 << 31) | (dim << 28) | (6 << 20) | (7 << 16) | (0 << 8) | rd]


def keep(reg):                                                  # SUATOM.ADD on image slot 0: uses the register, so its read stays
    return base('Suatom') | (0x48 << 36) | (3 << 33) | (reg << 20) | (7 << 16) | 255


def shader(*parts):
    insts = [mov(0, 0), mov(1, 0), mov(2, 0)]
    for part in parts: insts += part if isinstance(part, list) else [part]
    insts.append(base('Exit') | (7 << 16) | 15)
    words = []
    for i, inst in enumerate(insts):
        if i % 3 == 0: words.append(0)                          # Maxwell scheduling word
        words.append(inst)
    return struct.pack('<%dQ' % len(words), *words)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class QueryType(unittest.TestCase):
    def translate(self, code, tex=None):
        """The texture bindings as (type, c[2] sampler word's offset), and the translator's notes"""
        with tempfile.TemporaryDirectory(prefix='txq_type_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code)
            job = dict(key='k', stage='cs', code=str(work / 'k.bin'), tex=tex or {}, img={'0': dict(type='2d', fmt='R32Uint')}, local=[1, 1, 1], debug=True)
            (work / 'jobs.json').write_text(json.dumps([job]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            res = json.loads((work / 'k.res.json').read_text())
            return {((t['handle'] & 0x3fff) * 4, ((t['handle'] >> 14) & 0x3fff) * 4): t['type'] & (0xff | ARRAY) for t in res['textures']}, res['notes']

    def test_query_takes_the_type_of_the_sample(self):
        for dim, want in ((DIM_3D, TEXTURE_3D), (DIM_CUBE, TEXTURE_CUBE), (DIM_2D_ARRAY, TEXTURE_2D | ARRAY)):
            with self.subTest(dim=dim):
                textures, notes = self.translate(shader(query(8, SLOT_128), keep(8), sample(9, SLOT_128, dim), keep(9)))
                self.assertEqual(textures, {(SLOT_128, SAMPLER_6): want, (SLOT_128, SAMPLER_1): want})
                self.assertTrue(any("from the shader's other instructions" in n for n in notes), notes)

    def test_the_order_of_the_instructions_does_not_matter(self):
        textures, _ = self.translate(shader(sample(9, SLOT_128, DIM_3D), keep(9), query(8, SLOT_128), keep(8)))
        self.assertEqual(textures, {(SLOT_128, SAMPLER_6): TEXTURE_3D, (SLOT_128, SAMPLER_1): TEXTURE_3D})

    def test_a_query_with_nothing_else_on_its_slot_stays_2d(self):
        textures, notes = self.translate(shader(query(8, SLOT_128), keep(8)))
        self.assertEqual(textures, {(SLOT_128, SAMPLER_6): TEXTURE_2D})
        self.assertTrue(any('using 2D' in n for n in notes), notes)

    def test_a_sample_of_another_slot_gives_no_type(self):
        textures, _ = self.translate(shader(query(8, SLOT_128), keep(8), sample(9, SLOT_129, DIM_3D), keep(9)))
        self.assertEqual(textures, {(SLOT_128, SAMPLER_6): TEXTURE_2D, (SLOT_129, SAMPLER_1): TEXTURE_3D})

    def test_a_2d_sample_changes_nothing(self):
        textures, notes = self.translate(shader(query(8, SLOT_128), keep(8), sample(9, SLOT_128, DIM_2D), keep(9)))
        self.assertEqual(textures, {(SLOT_128, SAMPLER_6): TEXTURE_2D, (SLOT_128, SAMPLER_1): TEXTURE_2D})
        self.assertFalse(any('other instructions' in n for n in notes), notes)

    def test_two_shapes_on_one_slot_keep_the_guess(self):
        textures, notes = self.translate(shader(query(8, SLOT_128), keep(8), sample(9, SLOT_128, DIM_3D), keep(9), sample(9, SLOT_128, DIM_CUBE, SAMPLER_6 + 8), keep(9)))
        self.assertEqual(textures[(SLOT_128, SAMPLER_6)], TEXTURE_2D)
        self.assertFalse(any('other instructions' in n for n in notes), notes)

    def test_the_answer_table_comes_first(self):
        textures, notes = self.translate(shader(query(8, SLOT_128), keep(8), sample(9, SLOT_128, DIM_3D), keep(9)), tex={'128': 'cube'})
        self.assertEqual(textures[(SLOT_128, SAMPLER_6)], TEXTURE_CUBE)
        self.assertFalse(any('other instructions' in n for n in notes), notes)


if __name__ == '__main__':
    unittest.main()
