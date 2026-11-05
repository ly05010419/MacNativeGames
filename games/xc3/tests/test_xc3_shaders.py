"""Xenoblade Chronicles 3: shaders read from the game's containers are keyed and named as the runtime does. Needs no game files."""
import hashlib
import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import monolith_shaders as ms  # noqa: E402
import translate_shaders as ts  # noqa: E402


def shader(stage_word, code_len=0x80, ctl_len=0x880):
    """A synthetic control section and code area: NVN magics, control +0x20 its size, +0x704 the code length."""
    ctl = bytearray(ctl_len)
    ctl[:4] = ms.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, ctl_len)
    struct.pack_into('<II', ctl, 0x700, 0x30, code_len)
    code = bytearray(code_len)
    code[:4] = ms.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, stage_word)
    return bytes(ctl), bytes(code)


class KeyTests(unittest.TestCase):
    def test_key_is_the_runtimes(self):
        ctl, code = shader(5 << 10)
        # nvn_mtl.m shader_key: SHA-256 of the first ctl_key_len control bytes, then the code area's first control+0x704 bytes
        self.assertEqual(ms.shader_key(ctl + b'tail', code + b'tail'), hashlib.sha256(ctl + code).hexdigest())
        short = bytearray(ctl)
        struct.pack_into('<I', short, 0x20, 0x7d1)
        self.assertEqual(ms.ctl_key_len(bytes(short)), 0x7d1)
        struct.pack_into('<I', short, 0x20, 0x760)
        self.assertEqual(ms.ctl_key_len(bytes(short)), 0x880)   # below 0x7cc: the runtime hashes 0x880

    def test_stage_from_the_sph(self):
        self.assertEqual(ms.glslc_stage(*shader(1 << 10)), 'vs')
        self.assertEqual(ms.glslc_stage(*shader(5 << 10)), 'ps')


class NameTests(unittest.TestCase):
    def test_default_variant_has_the_runtimes_name(self):
        # totk_shader_rt.m totk_rt_prefetch's answers, as NSJSONSerialization writes them: 12,826 variants met in play
        self.assertEqual(ts.variant_name('k'), 'k-e594b1bc343e')

    def test_keys_ordered_numerically(self):
        self.assertEqual(ts.ns_json({'10': 1, '2': {}, 'a': 'b'}), '{"2":{},"10":1,"a":"b"}')


class ContainerTests(unittest.TestCase):
    def test_xbc1_and_cmp_streams_are_opened(self):
        ctl, code = shader(5 << 10)
        size = 0x48 + len(ctl) + len(code)
        out = bytearray(size)   # a one-stage GLSLC output: magic, +0x44 its length, the control section and its code after it
        out[:4] = ms.GLSLC_MAGIC
        struct.pack_into('<I', out, 0x44, size)
        out[0x48:0x48 + len(ctl)] = ctl
        out[0x48 + len(ctl):] = code
        inner = b'pad' + bytes(out)
        z = zlib.compress(inner)
        cmp = b'cmp\0zlib' + struct.pack('<2I', len(z), len(inner)) + z
        xbc = b'xbc1' + struct.pack('<3I', 1, len(cmp), len(zlib.compress(cmp))) + bytes(32) + zlib.compress(cmp)
        found = []
        ms.monolith_shaders(b'entry' + xbc + b'end', found)
        self.assertEqual([(st, ms.shader_key(c, k)) for st, c, k in found], [('ps', ms.shader_key(ctl, code))])


if __name__ == '__main__':
    unittest.main()
