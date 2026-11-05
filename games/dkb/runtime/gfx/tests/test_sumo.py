"""Sumo's shader descriptors, loose programs and compressed streams, without game data."""
from pathlib import Path
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def shader(stage):
    ctl, code = bytearray(0x850), bytearray(0x100)
    ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    if stage == 'cs':
        struct.pack_into('<3I', ctl, 0x748, 8, 1, 1)
        code[:4] = b'\x11\x07\x20\xe2'   # headerless compute code, as in the loose skinning shader
    else:
        code[:4] = split.GFX_MAGIC
        struct.pack_into('<I', code, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
        if stage == 'ps': struct.pack_into('<I', code, 0x48, 1)
    return bytes(ctl), bytes(code)


def descriptor(stage):
    ctl, code = shader(stage)
    r, co = 0x78, 0xd0
    ko = (co + len(ctl) + 31) & ~31
    d = bytearray(ko + len(code))
    struct.pack_into('<IIQ', d, 0, 0x12345678, 1, len(d))
    struct.pack_into('<QQII', d, r, co, ko, len(ctl), len(code))
    struct.pack_into('<Q', d, r + 0x28, r)
    d[r + 0x30:r + 0x40] = split.SUMO_NX
    d[co:co + len(ctl)], d[ko:] = ctl, code
    return d


def nvnbin(stage):
    ctl, code = shader(stage)
    d = bytearray(0x34 + len(ctl) + len(code))
    d[:8] = split.NVNSH + struct.pack('<H', 1)
    struct.pack_into('<I', d, 8, len(d))
    struct.pack_into('<I', d, 28, split.STAGES.index(stage))
    struct.pack_into('<4I', d, 32, 0x34, 0x34 + len(ctl), len(code), len(ctl))
    d[0x34:0x34 + len(ctl)], d[0x34 + len(ctl):] = ctl, code
    return d


def mux(*members):
    packed = [zlib.compress(m) for m in members]
    head = 44 + 4 * len(members)
    d = bytearray(head)
    d[:8] = split.SUMO_MUX
    struct.pack_into('<I', d, 8, head)
    d[17:19] = bytes((2, len(members)))
    for i, m in enumerate(members): struct.pack_into('<I', d, 24 + 8 * i, len(m))
    struct.pack_into('<' + 'I' * len(members), d, 40, *map(len, packed))
    return d + b''.join(packed)


class SumoTests(unittest.TestCase):
    def test_embedded_descriptors_use_container_relative_offsets(self):
        d = b'prefix' + descriptor('vs') + b'gap' + descriptor('ps')
        found = split.sumo_shaders(d, 'main')
        self.assertEqual([(st, ctl, code) for _, st, ctl, code in found], [('vs', *shader('vs')), ('ps', *shader('ps'))])
        pairs, cover = split.separable_pairs(split.uniq_keys(found))
        self.assertEqual(cover, [split.skey(*shader('ps'))])
        self.assertEqual(pairs, {(split.skey(*shader('vs')), cover[0])})

    def test_concatenated_loose_records_and_headerless_compute(self):
        found = split.sumo_shaders(nvnbin('vs') + nvnbin('cs'), '/programs')
        self.assertEqual([(st, ctl, code) for _, st, ctl, code in found], [('vs', *shader('vs')), ('cs', *shader('cs'))])

    def test_compressed_members_reach_the_file_scanner(self):
        for data in [mux(descriptor('vs')), mux(descriptor('vs'), nvnbin('ps')), nvnbin('cs'), descriptor('ps')]:
            with self.subTest(header=data[:8]), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'no_extension'
                path.write_bytes(data)
                _, keys, _, extra = split.scan_file((str(path), path.name))
                self.assertEqual(extra, {})
                self.assertTrue(keys)
                self.assertTrue(all(not split.check(w, s, c, k) for w, s, c, k in keys.values()))

    def test_nested_streams_keep_the_depth_limit(self):
        d = descriptor('vs')
        for _ in range(split.NEST + 1): d = mux(d)
        with self.assertRaisesRegex(SystemExit, 'archives nested more than'):
            split.scan_blob(d, '/nested', [])

    def test_streams_keep_the_parent_archive_budget(self):
        with self.assertRaisesRegex(SystemExit, 'archive members add up to more than'):
            split.scan_blob(mux(descriptor('vs')), '/member', [], left=[1, 1])

    def test_descriptor_bounds_version_and_shader_validation(self):
        for offset, fmt, value in [(4, '<I', 2), (0x78, '<Q', 0xffffffff), (0x78 + 8, '<Q', 0xd0), (0x78 + 16, '<I', 0x10), (0xd0 + 0x704, '<I', 8)]:
            d = descriptor('vs'); struct.pack_into(fmt, d, offset, value)
            with self.subTest(offset=offset), self.assertRaises(SystemExit): split.sumo_shaders(d, '/bad')

    def test_descriptor_whose_container_does_not_fit_is_a_literal(self):
        for offset, value in [(8, 0xffffffff), (0x78 + 0x28, 0xffffffff)]:
            d = descriptor('vs'); struct.pack_into('<Q', d, offset, value)
            with self.subTest(offset=offset): self.assertEqual(split.sumo_shaders(d, '/main'), [])
        for d in [descriptor('vs')[:-1], split.SUMO_NX, bytes(0x40) + split.SUMO_NX + bytes(0x40)]:
            self.assertEqual(split.sumo_shaders(d, '/main'), [])

    def test_loose_record_bounds_version_and_stage(self):
        for offset, value in [(28, 99), (28, 4), (32, 0xffffffff), (36, 0x34), (44, 0x10)]:
            d = nvnbin('vs'); struct.pack_into('<I', d, offset, value)
            with self.subTest(offset=offset), self.assertRaises(SystemExit): split.sumo_shaders(d, '/bad')
        d = nvnbin('vs'); struct.pack_into('<H', d, 6, 2)
        with self.assertRaises(SystemExit): split.sumo_shaders(d, '/version')

    def test_loose_record_that_does_not_fit_is_a_literal(self):
        for size in [0, 0x33, 0xffffffff]:
            d = nvnbin('vs'); struct.pack_into('<I', d, 8, size)
            with self.subTest(size=size): self.assertEqual(split.sumo_shaders(d, '/main'), [])
        for d in [nvnbin('vs')[:-1], split.NVNSH, b'code' + split.NVNSH + bytes(0x40)]:
            self.assertEqual(split.sumo_shaders(d, '/main'), [])

    def test_compressed_lengths_and_trailing_data(self):
        for offset, value in [(8, 0), (24, 1), (24, 0xffffff), (40, 1)]:
            d = mux(descriptor('vs')); struct.pack_into('<I', d, offset, value)
            with self.subTest(offset=offset), self.assertRaises(SystemExit): list(split.sumo_members(d, '/bad'))
        for d in [mux(descriptor('vs'))[:-1], mux(descriptor('vs')) + b'trailing', split.SUMO_MUX]:
            with self.assertRaises(SystemExit): list(split.sumo_members(d, '/cut'))
        d = mux(descriptor('vs')); d[18] = 3
        with self.assertRaises(SystemExit): list(split.sumo_members(d, '/layout'))

    def test_unrelated_bytes_and_unreadable_file(self):
        self.assertEqual(split.sumo_shaders(b'unrelated Ninty_NX string and NVNsh name', '/other'), [])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'bad.bin'; d = nvnbin('vs'); struct.pack_into('<H', d, 6, 2); path.write_bytes(d)
            with contextlib.redirect_stdout(io.StringIO()): result = split.scan_file((os.fspath(path), path.name))
            self.assertEqual(result[1], {})
            self.assertEqual(result[3][0], 'unreadable')


if __name__ == '__main__': unittest.main()
