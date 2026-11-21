"""Unreal's engine region found without a build ID (dm/tools/ue_region.py) on synthetic images, no game files: the option's name, nninitStartup (a b to the engine's function, or
a bl to it), its call of nn::os::AllocateAddressRegion through the PLT and the store of the size beside the base variable before it, in the forms the Unreal mains have; a broken
link gates nothing, with a [RED] reason once the option is there."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import ADRP, ADDI, B, LDRX, NOP, STRX
import ue_region

RET = 0xd65f03c0
TEXT, RO, DATA, END = 0x8000, 0x8000, 0xa000, 0xc000
NNINIT, ENGINE, PLT, SLOT, VAR = 0x1000, 0x1100, 0x7000, 0xa800, 0xa200   # VAR: the region's base variable, its size at VAR + 8
MOV_X1_62GB, MOV_X8_62GB = 0xb26113e1, 0xb26113e8   # orr x1 / x8, xzr, #0xf80000000
CSEL_X1_X8_X9_EQ = 0x9a890101


def bl(frm, to): return B(frm, to) | 0x80000000
def cbnz(rt, frm, to): return 0xb5000000 | (((to - frm) // 4) & 0x7ffff) << 5 | rt
def put(img, at, *words): struct.pack_into('<' + 'I' * len(words), img, at, *words)
def at(a, words): return [w(a + 4 * k) if callable(w) else w for k, w in enumerate(words)]   # words that need their own address (branches, adrp) as functions of it


def csel(call_at=None):
    """Unreal Engine 4.27 / 5: the size from the option (0 = absent) rounded up, or the default, by csel; str x1, then the call"""
    return [lambda a: ADRP(10, a, VAR), lambda a: ADRP(0, a, VAR), ADDI(0, 0, VAR & 0xfff), LDRX(8, 10, (VAR + 8 & 0xfff) // 8), 0x913ffd09, 0xf100011f, MOV_X8_62GB,
            0x9274cd29, CSEL_X1_X8_X9_EQ, STRX(1, 10, (VAR + 8 & 0xfff) // 8), lambda a: bl(a, call_at or PLT), RET]


def flag():
    """Unreal Engine 4.26 (Pikmin 4): the default only while the variable is 0, str x8 with x1 set from the same constant"""
    return [lambda a: ADRP(22, a, VAR), LDRX(8, 22, (VAR + 8 & 0xfff) // 8), lambda a: cbnz(8, a, a + 32), MOV_X8_62GB, lambda a: ADRP(0, a, VAR), ADDI(0, 0, VAR & 0xfff),
            MOV_X1_62GB, STRX(8, 22, (VAR + 8 & 0xfff) // 8), lambda a: bl(a, PLT), RET]


def image(engine, option=True, called=False):
    img = bytearray(END)
    if option: img[0x8400:0x8400 + 30] = b'SwitchVirtualAddressSpaceSize\0'
    # MOD0, the dynamic table: one JUMP_SLOT relocation of AllocateAddressRegion, and nninitStartup defined
    struct.pack_into('<I', img, 4, 0x20); img[0x20:0x24] = b'MOD0'; struct.pack_into('<i', img, 0x24, 0xb800 - 0x20)
    for k, (tag, v) in enumerate(((20, 7), (23, 0x9000), (2, 24), (6, 0x9100), (5, 0x9200), (0, 0))): struct.pack_into('<qQ', img, 0xb800 + 16 * k, tag, v)
    struct.pack_into('<QQq', img, 0x9000, SLOT, 1 << 32 | 1026, 0)
    names = b'\0' + ue_region.ALLOCATE.encode() + b'\0nninitStartup\0'
    img[0x9200:0x9200 + len(names)] = names
    struct.pack_into('<IBBHQQ', img, 0x9100 + 24, 1, 0x12, 0, 0, 0, 0)
    struct.pack_into('<IBBHQQ', img, 0x9100 + 48, 2 + len(ue_region.ALLOCATE), 0x12, 0, 1, NNINIT, 4 if not called else 12)
    put(img, PLT, ADRP(16, PLT, SLOT), 0xf9400211 | ((SLOT & 0xfff) // 8) << 10, ADDI(16, 16, SLOT & 0xfff), 0xd61f0220)
    put(img, NNINIT, *([B(NNINIT, ENGINE)] if not called else [NOP, bl(NNINIT + 4, ENGINE), RET]))
    put(img, ENGINE, *at(ENGINE, engine))
    put(img, 0x3000, bl(0x3000, 0x1800))   # the next function after the engine's
    return img


def find(img):
    off, reg, _, why = ue_region.find(bytes(img), TEXT, RO, DATA)
    return off, reg, why


class FindTest(unittest.TestCase):
    def test_csel_form(self):
        off, reg, why = find(image(csel()))
        self.assertEqual((off, reg), (ENGINE + 36, 1), why)

    def test_flag_form_stores_x8(self):
        off, reg, why = find(image(flag()))
        self.assertEqual((off, reg), (ENGINE + 28, 8), why)

    def test_engine_function_called_by_nninit(self):
        # Unreal Engine 4.18 (DRAGON QUEST XI S): nninitStartup calls the engine's function; x0 through mov x0, x19
        words = [lambda a: ADRP(25, a, VAR), LDRX(1, 25, (VAR + 8 & 0xfff) // 8), lambda a: cbnz(1, a, a + 12), 0xb25e03e1, STRX(1, 25, (VAR + 8 & 0xfff) // 8),
                 lambda a: ADRP(19, a, VAR), ADDI(19, 19, VAR & 0xfff), 0xaa1303e0, lambda a: bl(a, PLT), RET]   # mov x1, #0x400000000; mov x0, x19
        off, reg, why = find(image(words, called=True))
        self.assertEqual((off, reg), (ENGINE + 16, 1), why)

    def test_no_option_is_not_red(self):
        off, _, why = find(image(csel(), option=False))
        self.assertIsNone(off)
        self.assertFalse(why.startswith('[RED] '), why)

    def test_same_constant_set_after_the_store(self):
        # TRIANGLE STRATEGY (Unreal Engine 4.27): str x8 of the default, then mov x1 of the same constant: the instruction the rewriter makes mov x1, x8
        words = flag()
        words[6:8] = [words[7], words[6]]
        off, reg, x1_mov, why = ue_region.find(bytes(image(words)), TEXT, RO, DATA)
        self.assertEqual((off, reg, x1_mov), (ENGINE + 24, 8, ENGINE + 28), why)

    def test_flag_and_csel_forms_rewrite_nothing(self):
        for words in (flag(), csel()):
            self.assertIsNone(ue_region.find(bytes(image(words)), TEXT, RO, DATA)[2])

    def test_x1_written_after_the_store_is_red(self):
        words = flag()
        words[6:8] = [words[7], 0xb25e03e1]   # another constant after the str (mov x1, #0x400000000): the request would not take the size
        off, _, why = find(image(words))
        self.assertIsNone(off)
        self.assertTrue(why.startswith('[RED] ') and 'x1 written after' in why, why)

    def test_size_register_written_after_the_store_is_red(self):
        words = flag()
        words[6:8] = [words[7], MOV_X8_62GB, MOV_X1_62GB]   # x8 set again after the str: mov x1, x8 would miss the gate's size
        off, _, why = find(image(words))
        self.assertIsNone(off)
        self.assertTrue(why.startswith('[RED] '), why)

    def test_store_not_beside_the_base_is_red(self):
        words = csel()
        words[9] = STRX(1, 10, (VAR + 16 & 0xfff) // 8)
        off, _, why = find(image(words))
        self.assertIsNone(off)
        self.assertTrue(why.startswith('[RED] ') and 'no store of the size' in why, why)

    def test_two_reservations_are_red(self):
        off, _, why = find(image(csel()[:-1] + csel()))
        self.assertIsNone(off)
        self.assertTrue(why.startswith('[RED] ') and '2 reservations' in why, why)


if __name__ == '__main__':
    unittest.main()
