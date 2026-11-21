"""NUP classification and build routing with small synthetic packages."""
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import build_app
from tools.builder_jobs.classify import report
from nup import format as fmt

POKOPIA = '0400A3A01EE4C000'
KART_WORLD = '0400C3F00006E000'
ROMFS = b'synthetic RomFS payload'


def align16(size):
    return (size + 15) & ~15


def make_update(path, title, version):
    npdm = bytearray(0xA0)
    npdm[:4] = b'META'
    struct.pack_into('<II', npdm, 0x70, 0x80, 0x20)
    npdm[0x80:0x84] = b'ACI0'
    struct.pack_into('<Q', npdm, 0x90, int(title, 16))

    pkg = fmt.pack_pkg(fmt.Package(title_id=int(title, 16), version=version, kind=fmt.KIND_UPDATE,
                                  flags=fmt.PKG_FLAG_STANDALONE_UPDATE, exefs_dir=1, romfs_dir=2))
    dir_names = (b'', b'exefs', b'romfs')
    files = ((b'main.npdm', npdm), (b'sample.bin', ROMFS))
    pool = b''.join(dir_names) + b''.join(name for name, _ in files)
    dirs = b''.join(fmt._dir_s.pack(offset, len(name), first, count, 0)
                    for name, offset, first, count in zip(dir_names, (0, 0, 5), (0, 2, 3), (2, 1, 1)))
    children = struct.pack('<4I', 1, 2, 3, 4)
    pool_offset = fmt._toc_head_s.size + len(pkg) + len(dirs) + 2 * fmt.FILE_SIZE + len(children)
    blob_start = align16(fmt.HEADER_SIZE + pool_offset + len(pool))

    name_offset = sum(map(len, dir_names))
    file_offset = blob_start
    records = bytearray()
    blobs = bytearray()
    for name, data in files:
        records += fmt._file_s.pack(name_offset, len(name), len(data), file_offset, len(data),
                                    hashlib.sha256(data).digest(), fmt.CODEC_RAW, 0, 0)
        blobs += data + bytes(align16(len(data)) - len(data))
        name_offset += len(name)
        file_offset += align16(len(data))
    toc = fmt._toc_head_s.pack(fmt.TOC_MAGIC, 1, 3, 2, pool_offset, len(pool), 0)
    toc += pkg + dirs + records + children + pool
    head = fmt.pack_super(fmt.MAGIC, fmt.HEADER_SIZE, 1, fmt.HEADER_SIZE, len(toc), hashlib.sha256(toc).digest(),
                          blob_start, file_offset, 1, 0, bytes(16))
    path.write_bytes(head + toc + bytes(blob_start - len(head) - len(toc)) + blobs)


class TestNup(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / 'Ocarina of Time.nup'
        self.work = self.root / 'work'

    def test_pokopia_is_read_from_contents_and_routed_to_ns2(self):
        make_update(self.source, POKOPIA, 655360)
        plan = report([self.source])['plan']
        self.assertEqual((plan['game'], plan['update_version']), ('pkp', 655360))
        self.assertFalse(plan['needs_keys'])
        self.assertEqual(plan['blocks'], [])

        def check_dump(args, dump):
            tree = dump / 'updates/655360'
            self.assertEqual(build_app.npdm_program_id(tree / 'exefs/main.npdm'), POKOPIA)
            self.assertEqual((tree / 'romfs/sample.bin').read_bytes(), ROMFS)
            self.assertEqual(args.out, self.root / 'Pokémon Pokopia.app')
            self.assertEqual(dump.parent, self.work)

        with mock.patch.object(build_app, 'main_ns2', side_effect=check_dump) as native:
            build_app.main([str(self.source), '--work', str(self.work)])
        native.assert_called_once()
        self.assertEqual(list(self.work.iterdir()), [])

    def test_unsupported_title_is_not_routed_to_oot(self):
        make_update(self.source, KART_WORLD, 917504)
        got = report([self.source])
        self.assertEqual(got['items'][0]['title'], KART_WORLD)
        self.assertIsNone(got['plan']['base'])
        self.assertIn(KART_WORLD, got['plan']['blocks'][0])
        with self.assertRaisesRegex(build_app.BuildError, KART_WORLD):
            build_app.main([str(self.source)])

    def test_unsupported_version_stops_the_plan(self):
        make_update(self.source, POKOPIA, 655361)
        plan = report([self.source])['plan']
        self.assertIsNone(plan['base'])
        self.assertIn('655360', plan['blocks'][0])

    def test_oot_uses_original_importer(self):
        make_update(self.source, build_app.OOT_TITLE, 131072)
        self.assertEqual(report([self.source])['plan']['game'], 'oot')
        with mock.patch.object(build_app, 'build_oot') as oot:
            build_app.main([str(self.source)])
        self.assertEqual(oot.call_args.args[1], self.source)

    def test_corrupt_toc_blocks_classification(self):
        # The header and TOC digest are valid, but the TOC is too short to unpack.
        toc = b'bad'
        blob_start = align16(fmt.HEADER_SIZE + len(toc))
        head = fmt.pack_super(fmt.MAGIC, fmt.HEADER_SIZE, 1, fmt.HEADER_SIZE, len(toc), hashlib.sha256(toc).digest(),
                              blob_start, blob_start, 0, 0, bytes(16))
        self.source.write_bytes(head + toc + bytes(blob_start - len(head) - len(toc)))
        plan = report([self.source])['plan']
        self.assertIsNone(plan['base'])
        self.assertTrue(plan['blocks'])

    def test_bad_member_digest_stops_before_native_build_and_cleans_stage(self):
        make_update(self.source, POKOPIA, 655360)
        original = self.source.read_bytes()
        self.source.write_bytes(original.replace(ROMFS, b'x' * len(ROMFS)))
        with mock.patch.object(build_app, 'main_ns2') as native:
            with self.assertRaisesRegex(build_app.BuildError, 'NUP file hash mismatch'):
                build_app.main([str(self.source), '--work', str(self.work)])
        native.assert_not_called()
        self.assertEqual(list(self.work.iterdir()), [])
