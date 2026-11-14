"""Monolith archive boundaries and multi-stage programs, without game data."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def program(stages, extra=0):
    controls, codes = [], []
    for stage in stages:
        ctl, code = bytearray(0x880 + extra), bytearray(0x100)
        ctl[:4] = split.CTL_MAGIC
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        struct.pack_into('<I', ctl, 0x704, len(code))
        code[:4] = split.GFX_MAGIC
        struct.pack_into('<I', code, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
        controls.append(bytes(ctl)); codes.append(bytes(code))
    # One SLCT, two identical variants; each vertex bundle includes geometry.
    slct = bytearray(72 + 16 + 2 * 52)
    slct[:4] = b'SLCT'
    struct.pack_into('<2I', slct, 16, 2, 72)
    stored = [(len(c) + 7) & ~7 for c in controls]
    struct.pack_into('<4I', slct, 40, 0, sum(stored) * 2, 0, sum(map(len, codes)) * 2)
    for i in range(2):
        p = 88 + 52 * i
        struct.pack_into('<2I', slct, 72 + 8 * i, p, 52)
        struct.pack_into('<2I', slct, p + 24, 32, 1)
        vs = sum(len(c) for s, c in zip(stages, codes) if s != 'ps')
        vc = sum(n for s, n in zip(stages, stored) if s != 'ps')
        struct.pack_into('<5I', slct, p + 32, 1, vs, len(codes[-1]), vc, stored[-1])
    head = bytearray(80); head[:4] = b'HCPS'
    struct.pack_into('<I', head, 4, 1010)
    struct.pack_into('<2I', head, 8, 72, 1)
    ctl = b''.join(c.ljust(n, b'\0') for c, n in zip(controls, stored)) * 2; code = b''.join(codes) * 2
    struct.pack_into('<6I', head, 24, 80, len(slct), 80 + len(slct) + len(ctl), len(code), 80 + len(slct), len(ctl))
    return bytes(head) + slct + ctl + code, controls, codes


class MonolithTests(unittest.TestCase):
    def test_loose_containers_reach_the_archive_decoder(self):
        data, controls, codes = program(['vs', 'ps'])
        packed = zlib.compress(data, level=0)
        xbc = struct.pack('<4s4I28s', b'xbc1', 1, len(data), len(packed), 0, b'loose') + packed
        cmp = struct.pack('<8s2I', b'cmp\0zlib', len(packed), len(data)) + packed
        expected = {split.skey(c, k) for c, k in zip(controls, codes)}
        with tempfile.TemporaryDirectory() as tmp:
            for body in (data, xbc, b'DRSM' + bytes(60) + xbc + xbc, cmp):
                with self.subTest(magic=body[:4]):
                    path = Path(tmp) / 'resource.bin'   # detection uses the header, not the suffix
                    path.write_bytes(body)
                    _, keys, pairs, extra = split.scan_file((str(path), 'resource.bin'))
                    self.assertEqual(set(keys), expected)
                    self.assertEqual(pairs, {(split.skey(controls[0], codes[0]), split.skey(controls[1], codes[1]))})
                    self.assertEqual(extra, {})

    def test_text_controls_advance_at_eight_byte_alignment(self):
        data, ctls, codes = program(['vs', 'ps'], extra=13)
        out = split.hcps_shaders(data, 0, '/test')
        self.assertEqual([(s, c, k) for _, s, c, k in out], list(zip(['vs', 'ps'], ctls, codes)) * 2)

    def test_embedded_geometry_and_variant_pairs(self):
        data, ctls, codes = program(['vs', 'gs', 'ps'])
        out = split.hcps_shaders(data, 0, '/test')
        self.assertEqual([(s, c, k) for _, s, c, k in out], list(zip(['vs', 'gs', 'ps'], ctls, codes)) * 2)
        self.assertEqual(split.var_pairs(out), {(split.skey(ctls[0], codes[0]), split.skey(ctls[2], codes[2]))})

    def test_xbc_payload_is_scanned_only_after_decoding(self):
        data = program(['vs', 'ps'])[0]
        # A stored deflate block leaves HCPS visible inside the compressed stream.
        packed = zlib.compress(data, level=0)
        wrapper = struct.pack('<4s4I28s', b'xbc1', 1, len(data), len(packed), 0, b'test') + packed
        self.assertIn(b'HCPS', packed)
        out = []; split.monolith_shaders(wrapper, '/test', out)
        self.assertEqual(len(out), 4)

    def test_standalone_slct_uses_its_own_relative_offsets(self):
        data, ctls, codes = program(['vs', 'ps'])
        body = bytearray(data[80:])
        struct.pack_into('<I', body, 4, 0x1003)
        c = len(data[80:80 + 192])
        # The synthetic fixture's SLCT occupies 192 bytes; control and code
        # sections now use offsets from that SLCT rather than separate bases.
        struct.pack_into('<I', body, 40, c)
        struct.pack_into('<I', body, 48, c + sum(map(len, ctls)) * 2)
        out = []; split.scan_blob(bytes(body), 'main', out)
        self.assertEqual([(s, ctl, code) for _, s, ctl, code in out], list(zip(['vs', 'ps'], ctls, codes)) * 2)

    def test_effect_glslc_output_inside_xbc(self):
        _, controls, codes = program(['vs', 'ps'])
        data = bytearray(0x70) + controls[0] + codes[0]
        data[:4] = bytes.fromhex('91688619')
        struct.pack_into('<I', data, 0x44, len(data))
        packed = zlib.compress(data, level=0)
        wrapper = struct.pack('<4s4I28s', b'xbc1', 1, len(data), len(packed), 0, b'effect') + packed
        out = []; split.monolith_shaders(wrapper, '/effect', out)
        self.assertEqual([(s, c, k) for _, s, c, k in out], [('vs', controls[0], codes[0])])

    def test_effect_cmp_zlib_nested_inside_xbc(self):
        data, controls, codes = program(['vs', 'ps'])
        packed = zlib.compress(data, level=0)
        cmp = struct.pack('<8s2I', b'cmp\0zlib', len(packed), len(data)) + packed
        packed = zlib.compress(cmp, level=0)
        xbc = struct.pack('<4s4I28s', b'xbc1', 1, len(cmp), len(packed), 0, b'effect') + packed
        out = []; split.monolith_shaders(xbc, '/effect', out)
        self.assertEqual([(s, c, k) for _, s, c, k in out], list(zip(['vs', 'ps'], controls, codes)) * 2)

    def test_effect_cmp_zlib_checks_both_sizes(self):
        packed = zlib.compress(b'test')
        with self.assertRaisesRegex(SystemExit, 'stored/raw sizes'):
            split.monolith_cmp_data(struct.pack('<8s2I', b'cmp\0zlib', len(packed), 5) + packed, 0)

    def test_source_only_hcps_has_no_nvn_binaries(self):
        data = bytearray(program(['vs', 'ps'])[0])
        struct.pack_into('<4I', data, 32, 0, 0, 0, 0)
        self.assertEqual(split.hcps_shaders(data, 0, '/source'), [])

    def test_truncated_glslc_magic_is_not_a_program(self):
        out = []; split.monolith_shaders(bytes.fromhex('91688619'), '/truncated', out)
        self.assertEqual(out, [])

    def test_bad_section_bounds_are_rejected(self):
        data = bytearray(program(['vs', 'ps'])[0])
        struct.pack_into('<I', data, 36, len(data))
        with self.assertRaisesRegex(SystemExit, 'out of bounds'):
            split.hcps_shaders(data, 0, '/test')

    def test_arh_wrapper_is_outside_stored_size(self):
        strings = b'\0.bin\0' + struct.pack('<I', 0)
        nodes = [(-1, -1)] * 98; nodes[0] = (0, -1); nodes[97] = (-1, 0)
        trie = b''.join(struct.pack('<ii', *n) for n in nodes)
        so, no = 48, 48 + len(strings)
        fo = no + len(trie)
        # A non-word-aligned suffix table leaves the last two ID bytes plain.
        mask = bytes.fromhex('12345678')
        enc = lambda b: bytes(x ^ mask[i & 3] for i, x in enumerate(b[:len(b) & ~3])) + b[len(b) & ~3:]
        header = struct.pack('<4s9I8x', b'arh1', len(strings), len(nodes), so, len(strings), no, len(trie), fo, 1, int.from_bytes(mask, 'little') ^ 0xf3f35353)
        data = header + enc(strings) + enc(trie) + struct.pack('<Q4I', 32, 100, 200, 2, 0)
        self.assertEqual(split.arh_entries(data), [('a.bin', 32, 148)])


if __name__ == '__main__':
    unittest.main()
