"""The pak key search on a synthetic main: a key whose halves are two separate 16-byte literals, and one stored whole; no game files or keys."""
from pathlib import Path
import os
import struct
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak


def adrp(rd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12 & 0x1FFFFF
    return 0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd


def ldr_q(rt, rn, off):
    return 0x3DC00000 | (off // 16) << 10 | rn << 5 | rt


STP_Q0_Q1_X0, RET = 0xAD000400, 0xD65F03C0


def pak(path, key, mount=b'../../../\0'):
    """A version 11 pak footer whose index starts with the mount point, encrypted with key (the 0xcc footer of Unreal Engine 4.27)"""
    index = struct.pack('<i', len(mount)) + mount
    index += bytes(-len(index) % 16)
    enc = aes_encrypt(key, index)
    foot = bytes(16) + b'\x01' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 11, 0, len(enc)) + bytes(20) + bytes(32 * 5)
    path.write_bytes(enc + foot)


def aes_encrypt(key, data):
    import ctypes
    cc = ctypes.CDLL('/usr/lib/libSystem.B.dylib').CCCrypt
    out = ctypes.create_string_buffer(len(data)); moved = ctypes.c_size_t()
    assert cc(0, 0, 2, key, len(key), None, data, len(data), out, len(data), ctypes.byref(moved)) == 0
    return out.raw[:moved.value]


@unittest.skipUnless(sys.platform == 'darwin', 'CommonCrypto')
class KeySearch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.key = bytes(range(7, 39))

    def image(self, split):
        """text (0x1000 bytes) + read-only data: the callback's code in text, the key's halves 0x2350 bytes apart in it, or the key stored whole"""
        lo = 0x1000
        ro = bytearray(os.urandom(0x4000))
        a, b = lo + 0x100, lo + 0x2450
        if split: ro[a - lo:a - lo + 16], ro[b - lo:b - lo + 16] = self.key[:16], self.key[16:]
        else: ro[a - lo:a - lo + 32] = self.key
        code = [adrp(8, 0x40, a), adrp(9, 0x44, b), ldr_q(0, 8, a & 0xFFF), ldr_q(1, 9, b & 0xFFF), STP_Q0_Q1_X0, RET] if split else [RET]
        text = bytearray(lo); text[0x40:0x40 + 4 * len(code)] = struct.pack(f'<{len(code)}I', *code)
        return bytes(text + ro), lo, lo + len(ro)

    def test_split_literals(self):
        img, lo, hi = self.image(True)
        self.assertEqual(uepak.code_keys(img, lo), [self.key])
        p = Path(self.tmp.name) / 'a.pak'; pak(p, self.key)
        self.assertEqual(uepak.find_key(img, lo, hi, str(p)), self.key)

    def test_whole_window(self):
        img, lo, hi = self.image(False)
        self.assertEqual(uepak.code_keys(img, lo), [])
        p = Path(self.tmp.name) / 'a.pak'; pak(p, self.key)
        self.assertEqual(uepak.find_key(img, lo, hi, str(p)), self.key)

    def test_clobbered_base(self):
        """A register written between the adrp and the load holds no page any more"""
        img, lo, hi = self.image(True)
        w = list(struct.unpack_from('<6I', img, 0x40)); w.insert(2, 0xD2800008)   # mov x8, #0
        img = img[:0x40] + struct.pack('<7I', *w) + img[0x40 + 28:]
        self.assertEqual(uepak.code_keys(img, lo), [])


if __name__ == '__main__':
    unittest.main()
