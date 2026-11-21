"""Unreal's FMallocBinned3 found without a build ID (dm/tools/ue_malloc.py) on synthetic images, no game files: the name, its getter, the vtable (in place or through
relative relocations), the constructor and its one call; a broken link changes nothing, with a [RED] reason once the name is there."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import ADRP, ADDI, B, NOP, STRX
import ue_malloc

RET = 0xd65f03c0
TEXT, RO, DATA, END = 0x8000, 0x8000, 0xa000, 0xc000
NAME, VT, STUB = 0x8100, 0xa110, 0x1500   # the string, the vtable's address point, a trivial slot
GETTER, DTOR0, DTOR1, CTOR, CALLER = 0x1000, 0x1100, 0x1200, 0x1300, 0x2000


def bl(frm, to): return B(frm, to) | 0x80000000
def put(img, at, *words): struct.pack_into('<' + 'I' * len(words), img, at, *words)
def store_vt(at): return [ADRP(8, at, VT), ADDI(8, 8, VT & 0xfff), STRX(8, 0), RET]   # the vtable into [x0]


def image(slot=19, calls=1, relocated=False, name=True):
    img = bytearray(END)
    if name: img[NAME:NAME + 16] = 'Binned3'.encode('utf-16-le') + b'\0\0'
    put(img, GETTER, ADRP(0, GETTER, NAME), ADDI(0, 0, NAME & 0xfff), RET)
    for f in (DTOR0, DTOR1, CTOR): put(img, f, *store_vt(f))
    put(img, STUB, RET)
    for k in range(calls): put(img, CALLER + 8 * k, NOP, bl(CALLER + 8 * k + 4, CTOR))
    slots = [DTOR0, DTOR1] + [STUB] * (slot - 2) + [GETTER] + [STUB] * 3
    if not relocated:
        for k, v in enumerate(slots): struct.pack_into('<Q', img, VT + 8 * k, v)
        return img
    # The vtable's words are 0 in place: R_AARCH64_RELATIVE relocations (RELA, in ro as the linker puts them) give them
    struct.pack_into('<I', img, 4, 0x20); img[0x20:0x24] = b'MOD0'; struct.pack_into('<i', img, 0x24, 0xb800 - 0x20)
    rela = 0x9000
    for k, v in enumerate(slots): struct.pack_into('<QQq', img, rela + 24 * k, VT + 8 * k, 1027, v)
    for k, (tag, v) in enumerate(((7, rela), (8, 24 * len(slots)), (0, 0))): struct.pack_into('<qQ', img, 0xb800 + 16 * k, tag, v)
    return img


def find(img): return ue_malloc.find(bytes(img), TEXT, RO, DATA)


class FindTest(unittest.TestCase):
    def test_found_in_place(self):
        call, vt, slot, why = find(image())
        self.assertEqual((call, vt, slot), (CALLER + 4, VT, 19), why)

    def test_found_through_relocations(self):
        call, vt, slot, why = find(image(slot=26, relocated=True))
        self.assertEqual((call, vt, slot), (CALLER + 4, VT, 26), why)

    def test_no_name_is_not_red(self):
        call, _, _, why = find(image(name=False))
        self.assertIsNone(call)
        self.assertFalse(why.startswith('[RED] '), why)

    def test_unknown_layout_is_red(self):
        call, _, _, why = find(image(slot=20))
        self.assertIsNone(call)
        self.assertTrue(why.startswith('[RED] ') and 'slot 20' in why, why)

    def test_two_calls_are_red(self):
        call, _, _, why = find(image(calls=2))
        self.assertIsNone(call)
        self.assertTrue(why.startswith('[RED] ') and '2 calls' in why, why)


if __name__ == '__main__':
    unittest.main()
