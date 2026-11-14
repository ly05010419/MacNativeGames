"""Unreal Engine 3 shader caches (Batman: Arkham Knight), without game data: package chunks, shader records and the key mask (shader_key_skip)."""
import os
from pathlib import Path
import struct
import sys
import unittest
import zlib

os.environ.setdefault('DM_GAME', '010015100B514000')
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split


def shader(seed, code_len=0x100):
    """A pixel shader: a code area (graphics header, SPH stage 5) and its 0x880-byte control section"""
    code = bytearray(code_len); code[:4] = shader_split.GFX_MAGIC; struct.pack_into('<I', code, 0x30, 5 << 10); code[0x80:0x84] = struct.pack('<I', seed)
    ctl = bytearray(0x880); ctl[:4] = shader_split.CTL_MAGIC; struct.pack_into('<I', ctl, 0x704, code_len); ctl[0x7d0:0x7e8] = bytes([seed & 0xff]) * 0x18
    return bytes(code), bytes(ctl)


def record(code, ctl, extra=False):
    """A cache record: u32 length, u32 code area length, u32 control length (one more u32 in the global cache), the code area, then the control section"""
    head = struct.pack('<III', len(code) + len(ctl) + 0x10, len(code), len(ctl)) + (b'\0\0\0\0' if extra else b'')
    return head + code + ctl


def package(body, block=0x40, compress=None):
    """An Unreal Engine 3 package of file version 864: a summary, then one compressed chunk of the body in blocks of `block` bytes"""
    blocks = [body[i:i + block] for i in range(0, len(body), block)]
    stored = [compress(b) if compress else b for b in blocks]
    chunk = shader_split.UE3_TAG + struct.pack('<III', block, sum(map(len, stored)), len(body)) + b''.join(struct.pack('<II', len(s), len(b)) for s, b in zip(stored, blocks))
    summary = shader_split.UE3_TAG + struct.pack('<HH', 864, 0x80e3) + b'summary.'
    return summary + chunk + b''.join(stored), summary


class Ue3CacheTests(unittest.TestCase):
    def setUp(self):
        self.real_oodle, self.real_skip = shader_split.oodle, shader_split.KEY_SKIP
        self.calls = []
        def fake_oodle(comp, n):   # stands in for the game's decoder in the Mac host (DM_OODLE_SERVE): the "compressed" block is the raw one behind a two-byte header
            self.calls.append(len(comp)); self.assertEqual(comp[:2], b'\x8c\x0a'); return comp[2:2 + n]
        shader_split.oodle = fake_oodle

    def tearDown(self):
        shader_split.oodle, shader_split.KEY_SKIP = self.real_oodle, self.real_skip

    def test_package_tag_tells_ue3_from_ue4(self):
        self.assertTrue(shader_split.ue3_package(shader_split.UE3_TAG + struct.pack('<HH', 864, 0x80e3)))
        self.assertFalse(shader_split.ue3_package(shader_split.UE3_TAG + struct.pack('<i', -7)))   # Unreal Engine 4's legacy file version

    def test_chunks_zlib_stored_and_oodle(self):
        body = b''.join(record(*shader(i)) for i in range(3))
        for compress, oodle_calls in ((zlib.compress, 0), (None, 0), (lambda b: b'\x8c\x0a' + b + b'pad', None)):
            self.calls.clear()
            d, summary = package(body, compress=compress)
            self.assertEqual(shader_split.ue3_body(d, 'p.xxx'), summary + body)
            if oodle_calls is not None: self.assertEqual(len(self.calls), oodle_calls)
            else: self.assertTrue(self.calls)

    def test_chunks_must_run_to_the_end(self):
        d, _ = package(record(*shader(1)))
        with self.assertRaisesRegex(SystemExit, 'no compressed chunk'):
            shader_split.ue3_body(d + b'trailing', 'p.xxx')

    def test_records_of_both_caches(self):
        shaders = [shader(i, 0x100 + 0x80 * (i % 2)) for i in range(4)]
        d = b'BMSG' + b'\0' * 12 + b''.join(record(c, k, extra=i % 2 == 0) for i, (c, k) in enumerate(shaders))
        got = shader_split.ue3_shaders(d, 'g.bin')
        self.assertEqual([(st, ctl, code) for _, st, ctl, code in got], [('ps', k, c) for c, k in shaders])
        self.assertTrue(all('#UE3@' in w for w, *_ in got))   # single-stage programs (separable_pairs)

    def test_key_skip_masks_the_compile_information(self):
        (c1, k1), (_, k2) = shader(1), shader(2)
        k2 = k1[:0x7d0] + k2[0x7d0:0x7e8] + k1[0x7e8:]   # the same section but its compile information
        shader_split.KEY_SKIP = (0, 0)
        self.assertNotEqual(shader_split.skey(k1, c1), shader_split.skey(k2, c1))
        self.assertFalse(shader_split.same_shader((k1, c1), (k2, c1)))
        shader_split.KEY_SKIP = (0x7d0, 0x18)
        self.assertEqual(shader_split.skey(k1, c1), shader_split.skey(k2, c1))
        self.assertTrue(shader_split.same_shader((k1, c1), (k2, c1)))
        self.assertEqual(shader_split.skey(k1, c1), shader_split.hashlib.sha256(k1[:0x7d0] + bytes(0x18) + k1[0x7e8:] + c1).hexdigest()[:16])


if __name__ == '__main__':
    unittest.main()
