"""Snowdrop's compressed shader records, without game data."""
from pathlib import Path
import os
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def shader(stage):
    ctl, code = bytearray(0x880), bytearray(0x100)
    ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    code[:4] = split.CS_MAGIC if stage == 'cs' else split.GFX_MAGIC
    if stage != 'cs':
        struct.pack_into('<I', code, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
    if stage == 'ps': struct.pack_into('<I', code, 0x48, 1)   # reads generic location 0
    return bytes(ctl), bytes(code)


def record(raw, flags=0x48, legacy=False):
    packed = zlib.compress(raw) if legacy else split.zstd_mod().compress(raw)
    return struct.pack('<5I', 0xabcd00c5 if legacy else 0xabcd00c6, flags, 0, len(raw), len(packed)) + packed


def archive(*records):
    return b'BERG' + struct.pack('<I', 22) + bytes(48) + b''.join(records)


class SnowdropTests(unittest.TestCase):
    def test_zlib_records_and_headerless_compute(self):
        ctl, code = map(bytearray, shader('cs'))
        ctl = ctl[:0x838]
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        struct.pack_into('<3I', ctl, 0x748, 8, 8, 1)
        code[:4] = b'\x11\x07\x20\xe2'
        data = archive(record(b'x' + ctl + code, legacy=True), record(b''.join(shader('ps'))))
        found = split.snowdrop_shaders(data, '/both encodings')
        self.assertEqual([s for _, s, _, _ in found], ['cs', 'ps'])
        self.assertEqual(found[0][2:], (bytes(ctl), bytes(code)))

    def test_zlib_cache_records_inside_zstd_archive_chunks(self):
        raw = record(b''.join(shader('ps')), legacy=True)
        data = archive(split.zstd_mod().compress(raw), record(b''.join(shader('vs'))))
        found = split.snowdrop_shaders(data, '/nested')
        self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps'])
        nested = [f for f in found if '#SDFZSTD@' in f[0]]
        self.assertEqual(len(nested), 1)
        self.assertEqual(nested[0][2:], shader('ps'))

    def test_wrapped_record_crossing_zstd_block_boundaries(self):
        # An incompressible cache crosses zstd blocks: its literal c5 header must
        # not be read directly across the outer frame's block headers.
        raw = record(random.Random(0).randbytes(0x50000) + b''.join(shader('ps')), legacy=True)
        data = archive(split.zstd_mod().compress(raw))
        found = split.snowdrop_shaders(data, '/large wrapped cache')
        self.assertEqual(len(found), 1)
        self.assertIn('#SDFZSTD@', found[0][0])
        self.assertEqual(found[0][2:], shader('ps'))

    def test_false_frame_magic_and_non_shader_chunks_are_ignored(self):
        data = archive(split.ZSTD_MAGIC + b'not a frame', split.zstd_mod().compress(b'other asset'))
        self.assertEqual(split.snowdrop_shaders(data, '/other assets'), [])

    def test_byte_packed_secondary_stages_and_separable_pairs(self):
        # A control can be at an unaligned offset; a single asset may hold two stages.
        raw = b'asset reflection\0x' + b''.join(shader('vs')) + b'variant' + b''.join(shader('ps'))
        data = archive(record(raw), record(b'x' + b''.join(shader('cs')), flags=0x404))
        found = split.snowdrop_shaders(data, '/arbitrary-name')
        self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps', 'cs'])
        for (_, stage, ctl, code) in found:
            self.assertEqual((ctl, code), shader(stage))
        self.assertEqual(split.var_pairs(found), set())
        self.assertEqual(len(split.separable_pairs(split.uniq_keys(found))[0]), 1)

    def test_scan_selects_by_berg_magic(self):
        data = archive(record(b''.join(shader('ps'))))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'without-extension'
            path.write_bytes(data)
            rel, keys, pairs, trees = split.scan_file((str(path), 'without-extension'))
        self.assertEqual(rel, 'without-extension')
        self.assertEqual(len(keys), 1)
        self.assertEqual(pairs, set())
        self.assertEqual(trees, {})

    def test_bare_frames_and_false_control_magics_are_not_shaders(self):
        data = archive(split.zstd_mod().compress(b''.join(shader('ps'))), record(b'other asset' + split.CTL_MAGIC + bytes(8)))
        self.assertEqual(split.snowdrop_shaders(data, '/other assets'), [])

    def test_record_bounds_and_decoded_length_are_checked(self):
        for legacy in (False, True):
            with self.subTest(legacy=legacy):
                data = archive(record(b''.join(shader('vs')), legacy=legacy))
                with self.assertRaisesRegex(SystemExit, 'stored bytes out of bounds'):
                    split.snowdrop_shaders(data[:-1], '/truncated')
                bad = bytearray(data)
                struct.pack_into('<I', bad, 56 + 12, 1)
                with self.assertRaisesRegex(SystemExit, 'expected 1 raw bytes'):
                    split.snowdrop_shaders(bytes(bad), '/bad raw size')

    def test_dlc_romfs_has_no_main_and_joins_the_shader_union(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            romfs, dlc = root / 'RomFS', root / 'aoc/7'
            romfs.mkdir(); dlc.mkdir(parents=True)
            (dlc / 'content').write_bytes(archive(record(b''.join(shader('ps')))))
            # Minimal uncompressed NSO with an empty code image; all shaders are in the DLC.
            nso = bytearray(0x1100)
            nso[:4] = b'NSO0'
            struct.pack_into('<3I', nso, 0x10, 0x100, 0, 0x1000)
            struct.pack_into('<3I', nso, 0x20, 0x1100, 0x1000, 0)   # empty ro and data after the text, as Horizon's loader requires
            struct.pack_into('<3I', nso, 0x30, 0x1100, 0x1000, 0)
            struct.pack_into('<I', nso, 0x60, 0x1000)
            main = root / 'main'; main.write_bytes(nso)
            outputs = [root / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
            result = subprocess.run([sys.executable, str(Path(split.__file__)), str(romfs), str(main), *map(str, outputs), str(dlc)],
                                    env=dict(os.environ, DM_GAME=split.GAME.id, J='1'), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('shaders: 1', result.stdout)
            self.assertIn('/aoc/7/content#SNOWDROP@', outputs[1].read_text())


if __name__ == '__main__':
    unittest.main()
