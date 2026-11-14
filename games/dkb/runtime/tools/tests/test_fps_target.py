"""A target frame rate the game steps by, found by its readers and stores (dm/tools/fps_target.py), on synthetic code, no game files: readers through the global itself (adrp + add) and
through its GOT slot (a relative relocation), the conversion into another register, too few readers, no store of 30, a dividend other than 60.0, and two fields that qualify."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import ADRP, ADDI, LDRX
import fps_target

RET = 0xd65f03c0
TEXT, RO, DATA, END = 0x8000, 0x8000, 0xa000, 0xc000
MOD0, DYN, RELA, GOT, GLOBAL, GLOBAL2, FIELD = 0x40, 0x9000, 0x9100, 0xa008, 0xa5d8, 0xa600, 0x1dc


def movz(d, v): return 0x52800000 | (1 << 21 if v > 0xffff else 0) | ((v >> 16 if v > 0xffff else v) & 0xffff) << 5 | d
def fmov_s_w(d, n): return 0x1E270000 | n << 5 | d
def scvtf(d, n): return 0x5E21D800 | n << 5 | d
def fdiv(d, n, m): return 0x1E201800 | m << 16 | n << 5 | d
def ldr_s(t, n, off): return 0xBD400000 | (off // 4) << 10 | n << 5 | t
def str_w(t, n, off): return 0xB9000000 | (off // 4) << 10 | n << 5 | t


def obj(at, g, got):
    """x8 = the object: the global's value, its address made by adrp + add, or read from its GOT slot (adrp + ldr)"""
    if got: return [ADRP(8, at, GOT), LDRX(8, 8, (GOT & 0xfff) // 8), LDRX(8, 8)]
    return [ADRP(8, at, g), ADDI(8, 8, g & 0xfff), LDRX(8, 8)]


def reader(at, g=GLOBAL, got=False, sixty=0x42700000, conv_to=0):
    w = obj(at, g, got)
    return w + [ldr_s(0, 8, FIELD), movz(9, sixty), fmov_s_w(1, 9), scvtf(conv_to, 0), fdiv(2, 1, conv_to), RET]


def store(at, value, g=GLOBAL, got=False): return [movz(9, value)] + obj(at + 4, g, got) + [str_w(9, 8, FIELD), RET]


def image(*seqs):
    img = bytearray(END)
    struct.pack_into('<I', img, 4, MOD0); img[MOD0:MOD0 + 4] = b'MOD0'; struct.pack_into('<i', img, MOD0 + 4, DYN - MOD0)
    struct.pack_into('<qQqQqQ', img, DYN, 7, RELA, 8, 24, 0, 0)             # DT_RELA, DT_RELASZ: one relocation
    struct.pack_into('<QQq', img, RELA, GOT, 1027, GLOBAL)                 # R_AARCH64_RELATIVE: the GOT slot holds the global's address
    at = 0x1000
    for s in seqs:
        words = s(at)
        struct.pack_into('<' + 'I' * len(words), img, at, *words)
        at += 4 * len(words) + 0x20
    return img


def find(img): return fps_target.find(bytes(img), TEXT, RO, DATA)


N = fps_target.MIN_READERS


class FindTest(unittest.TestCase):
    def test_readers_and_a_store_of_30(self):
        g, f, why = find(image(*[reader] * N, lambda a: store(a, 60), lambda a: store(a, 30)))
        self.assertEqual((g, f), (GLOBAL, FIELD), why)
        self.assertIn('set to 60 at', why)

    def test_through_the_got_slot(self):
        g, f, why = find(image(*[lambda a: reader(a, got=True)] * N, lambda a: store(a, 30, got=True)))
        self.assertEqual((g, f), (GLOBAL, FIELD), why)

    def test_conversion_into_another_register(self):
        g, f, why = find(image(*[lambda a: reader(a, conv_to=3)] * N, lambda a: store(a, 30)))
        self.assertEqual((g, f), (GLOBAL, FIELD), why)

    def test_too_few_readers(self):
        g, f, why = find(image(*[reader] * (N - 1), lambda a: store(a, 30)))
        self.assertIsNone(g, why)

    def test_no_store_of_30(self):
        g, f, why = find(image(*[reader] * N, lambda a: store(a, 60)))
        self.assertIsNone(g, why)

    def test_another_dividend(self):
        g, f, why = find(image(*[lambda a: reader(a, sixty=0x41f00000)] * N, lambda a: store(a, 30)))   # 30.0 / field
        self.assertIsNone(g, why)

    def test_two_fields_fail_closed(self):
        g, f, why = find(image(*[reader] * N, lambda a: store(a, 30), *[lambda a: reader(a, g=GLOBAL2)] * N, lambda a: store(a, 30, g=GLOBAL2)))
        self.assertIsNone(g)
        self.assertTrue(why.startswith('[RED] '), why)


if __name__ == '__main__':
    unittest.main()
