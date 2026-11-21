"""Gzip-wrapped shader sources preserve GLSLC stages/pairs and reject damaged or oversized streams. Synthetic bytes only (issue #497)."""
import contextlib
import gzip
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz
ss = fz.ss


def archive():
    """One synthetic GLSLC program in a Wii U8 archive, as in Shaders/Shaders.arc.gz."""
    shader = fz.glslc([fz.VS, fz.PS]); names = b'\0program.glslc\0'; data_offset = 0x60
    header = struct.pack('>4I', 0x55aa382d, 0x20, 24 + len(names), data_offset) + bytes(16)
    nodes = struct.pack('>6I', 0x01000000, 0, 2, 1, data_offset, len(shader)) + names
    return header + nodes + bytes(data_offset - len(header) - len(nodes)) + shader


class GzipSources(unittest.TestCase):
    def scan(self, data, name='Shaders/Shaders.arc.gz'):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(t) / 'source'; path.write_bytes(data)
            result = ss.scan_file((str(path), name))
        return result, log.getvalue()

    def test_magic_not_filename_selects_gzip(self):
        raw = archive(); (rel, keys, pairs, extra), log = self.scan(raw)
        self.assertEqual({value[1] for value in keys.values()}, {'vs', 'ps'})
        self.assertEqual(len(keys), 2); self.assertEqual(len(pairs), 1); self.assertEqual(log, '')
        for name in (rel, 'Shaders/opaque.bin'):
            with self.subTest(name=name):
                (got_rel, got_keys, got_pairs, got_extra), log = self.scan(gzip.compress(raw), name)
                self.assertEqual(got_rel, name); self.assertEqual(log, '')
                self.assertEqual({k: v[1:] for k, v in got_keys.items()}, {k: v[1:] for k, v in keys.items()})
                self.assertEqual(got_pairs, pairs); self.assertEqual(got_extra, extra)

    def test_gzip_timestamp_is_not_an_inner_compression_header(self):
        raw = archive(); expected = self.scan(raw)
        for magic in (ss.ZSTD_MAGIC, ss.LZ4_FRAME_MAGIC):
            with self.subTest(timestamp=magic.hex()):
                self.assertEqual(self.scan(gzip.compress(raw, mtime=int.from_bytes(magic, 'little'))), expected)

    def test_concatenated_members_form_one_shader_source(self):
        raw = archive(); cut = len(raw) // 2
        compressed = gzip.compress(raw[:cut]) + gzip.compress(raw[cut:])
        self.assertEqual(self.scan(compressed), self.scan(raw))

    def test_damaged_members_are_named_without_partial_shaders(self):
        raw = archive(); packed = gzip.compress(raw); bad_crc = bytearray(packed); bad_crc[-8] ^= 1
        for label, data in [('CRC', bytes(bad_crc)), ('footer', packed[:-4]), ('later member', packed + bytes(bad_crc)), ('trailing garbage', packed + b'garbage')]:
            with self.subTest(damage=label):
                (rel, keys, pairs, extra), log = self.scan(data)
                self.assertEqual((keys, pairs), ({}, set()))
                self.assertIsInstance(extra, tuple); self.assertEqual(extra[0], 'unreadable')
                self.assertIn('invalid gzip stream', extra[1]); self.assertIn(rel, extra[1])
                self.assertTrue(log.startswith('[RED] /Shaders/Shaders.arc.gz: invalid gzip stream'))

    def test_compressible_file_without_shaders_is_not_unreadable(self):
        data = gzip.compress(bytes(4 << 20))   # about 1000x, far beyond NEST times the file: an unreadable file would stop a registered game's build
        self.assertGreater(4 << 20, ss.NEST * len(data))
        (_, keys, pairs, extra), log = self.scan(data)
        self.assertEqual((keys, pairs, extra, log), ({}, set(), {}, ''))

    def test_decoded_byte_budget_allows_exact_limit(self):
        with mock.patch.object(ss, 'GZIP_FLOOR', 1 << 20):
            (_, keys, pairs, extra), log = self.scan(gzip.compress(bytes(1 << 20)))
        self.assertEqual((keys, pairs, extra, log), ({}, set(), {}, ''))

    def test_decoded_byte_budget_counts_all_members(self):
        for data in [gzip.compress(bytes((1 << 20) + 1)), gzip.compress(bytes(1 << 20)) + gzip.compress(b'x')]:
            with self.subTest(members=len(data)), mock.patch.object(ss, 'GZIP_FLOOR', 1 << 20):
                (_, keys, pairs, extra), log = self.scan(data)
                self.assertEqual((keys, pairs), ({}, set())); self.assertIsInstance(extra, tuple); self.assertEqual(extra[0], 'unreadable')
                self.assertIn('expands beyond', extra[1]); self.assertIn('0x100000', log)

    def test_cli_matches_uncompressed_archive_outputs(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t); rom = root / 'RomFS'; (rom / 'Shaders').mkdir(parents=True)
            source = rom / 'Shaders/Shaders.arc.gz'; main = root / 'main'; main.write_bytes(b'NSO0' + bytes(0xfc))
            results = []
            for label, data in [('plain', archive()), ('gzip', gzip.compress(archive()))]:
                source.write_bytes(data); dest = root / label; dest.mkdir()
                out = [dest / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
                proc = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main, *out], capture_output=True, text=True,
                                      env=dict(os.environ, DM_GAME='0100C1F0051B6000', J='2'))
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr); self.assertNotIn('[RED]', proc.stdout + proc.stderr)
                self.assertIn('shaders: 2 (', proc.stdout)
                results.append({str(p.relative_to(dest)): p.read_bytes() for p in dest.rglob('*') if p.is_file()})
            self.assertEqual(results[0], results[1])


if __name__ == '__main__': unittest.main()
