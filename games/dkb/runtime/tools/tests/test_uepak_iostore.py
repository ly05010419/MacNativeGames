"""IoStore shader resolution using synthetic TOCs, libraries and shader groups; no game files or keys."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak


def chunk_id(n, kind):
    return struct.pack('<QHBB', n, 0, 0, kind)


LIB = chunk_id(1, uepak.IO_SHADER_LIBRARY)
GROUP = chunk_id(2, uepak.IO_SHADER_CODE)
OTHER = chunk_id(3, uepak.IO_SHADER_CODE)


def library(groups):
    """One shader per group: (chunk ID, decoded bytes, stored bytes), frequency pixel."""
    n = len(groups)
    b = struct.pack('<II', 1, 1) + bytes(20) + struct.pack('<I', n) + bytes(20 * n)
    b += struct.pack('<I', n) + b''.join(gid for gid, _, _ in groups)
    b += struct.pack('<III', 1, 0, n) + struct.pack('<I', n)
    b += b''.join(struct.pack('<Q', 3 | i << 4) for i in range(n))
    b += struct.pack('<I', n)
    b += b''.join(struct.pack('<IIII', i, 1, len(raw), len(stored)) for i, (_, raw, stored) in enumerate(groups))
    return b + struct.pack(f'<I{n}I', n, *range(n))


def container(root, name, chunks, encrypted=False, compressed=False, version=8):
    """A minimal TOC with each chunk in a separate 4 KB block. Oodle's test stand-in is zlib."""
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    head = bytearray(144); head[:16] = uepak.TOC_MAGIC; head[16] = version
    struct.pack_into('<9I', head, 20, 144, len(chunks), len(chunks), 12, int(compressed), 32, 4096, 0, 1)
    head[80] = 2 if encrypted else 0
    ids, ranges, blocks, data = bytearray(), bytearray(), bytearray(), bytearray()
    for i, (gid, raw) in enumerate(chunks):
        stored = zlib.compress(raw) if compressed else raw
        ids += gid; ranges += (i * 4096).to_bytes(5, 'big') + len(raw).to_bytes(5, 'big')
        blocks += len(data).to_bytes(5, 'little') + len(stored).to_bytes(3, 'little') + len(raw).to_bytes(3, 'little') + bytes([int(compressed)])
        data += stored
    path.write_bytes(head + ids + ranges + blocks + (b'Oodle' + bytes(27) if compressed else b''))
    path.with_suffix('.ucas').write_bytes(data)
    return path


class IoStore(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name); self.romfs = self.root / 'RomFS'; self.romfs.mkdir()
        self.work = self.root / 'work'

    def run_job(self, decode=None):
        # The NSO and pak paths are outside this test's scope; TOC parsing, resolution and archive writing are real.
        with patch.object(uepak, 'nso_image', return_value=(b'', 0, 0)), patch.object(uepak, 'unreal_paks', return_value=[]):
            return uepak.job(str(self.romfs), 'unused-main', str(self.work), decode=decode or (lambda data, size: data))

    def shaders(self, entry):
        b = (self.work / 'archives' / entry['name']).read_bytes(); a: dict = uepak.archive(b)
        self.assertEqual(a['maps'], [(0, len(a['shaders']))])
        self.assertEqual(a['indices'], list(range(len(a['shaders']))))
        self.assertTrue(all(e[1] == e[2] and e[3] == 3 for e in a['shaders']))
        return [b[a['code'] + e[0]:a['code'] + e[0] + e[1]] for e in a['shaders']]

    def test_single_container_and_finish(self):
        raw = b'LSLG-single-container'
        container(self.romfs, 'Game.utoc', [(LIB, library([(GROUP, raw, raw)])), (GROUP, raw)])
        entries, records = self.run_job()
        self.assertEqual((len(entries), records), (1, 0)); self.assertEqual(self.shaders(entries[0]), [raw])
        out = self.root / 'finished'; self.assertEqual(uepak.finish(str(self.work), str(out)), [entries[0]['name']])
        self.assertEqual((out / entries[0]['name']).read_bytes(), (self.work / 'archives' / entries[0]['name']).read_bytes())
        self.assertEqual(json.loads((self.work / 'job.json').read_text())['records'], [])

    def test_patch_library_uses_base_and_patch_groups(self):
        base, updated = b'LSLG-unchanged', b'LSLG-updated'
        container(self.romfs, 'Game.utoc', [(LIB, library([(GROUP, base, base)])), (GROUP, base)])
        container(self.romfs, 'Game_0_P.utoc', [(LIB, library([(GROUP, base, base), (OTHER, updated, updated)])), (OTHER, updated)])
        entries, records = self.run_job()
        self.assertEqual((len(entries), records), (1, 0))
        self.assertEqual(entries[0]['pak'], 'Game_0_P.utoc'); self.assertEqual(self.shaders(entries[0]), [base, updated])

    def test_numeric_patch_priority_and_group_only_override(self):
        raw = b'LSLG-new'
        # Deliberately create in a different order from mount order; 10 must beat 2.
        container(self.romfs, 'Game_10_P.utoc', [(GROUP, raw)])
        container(self.romfs, 'Game_2_P.utoc', [(LIB, library([(GROUP, raw, raw)])), (GROUP, b'LSLG-old')])
        container(self.romfs, 'Game.utoc', [(LIB, library([(OTHER, b'obsolete', b'obsolete')]))])
        entries, _ = self.run_job()
        self.assertEqual(len(entries), 1); self.assertEqual(self.shaders(entries[0]), [raw])
        self.assertEqual(entries[0]['pak'], 'Game_2_P.utoc')

    def test_unnumbered_and_lowercase_patch_priority(self):
        for name in ('A_P.utoc', 'A_p.utoc'):
            with self.subTest(name=name), tempfile.TemporaryDirectory(dir=self.romfs) as td:
                root = Path(td); raw = b'LSLG-patch'
                container(root, 'Z.utoc', [(LIB, library([(GROUP, b'LSLG-base!', b'LSLG-base!')])), (GROUP, b'LSLG-base!')])
                container(root, name, [(LIB, library([(GROUP, raw, raw)])), (GROUP, raw)])
                entries, _ = self.run_job()
                self.assertEqual(self.shaders(entries[0]), [raw])

    def test_two_compression_layers_across_containers(self):
        raw = b'LSLG' + b'compressed shader' * 50; stored = zlib.compress(raw)
        self.assertLess(len(stored), len(raw))
        container(self.romfs, 'Game.utoc', [(GROUP, stored)], compressed=True)
        container(self.romfs, 'Game_0_P.utoc', [(LIB, library([(GROUP, raw, stored)]))], compressed=True)
        calls = []
        def decode(data, size):
            out = zlib.decompress(data); self.assertEqual(len(out), size); calls.append(size); return out
        entries, _ = self.run_job(decode)
        self.assertEqual(self.shaders(entries[0]), [raw]); self.assertEqual(len(calls), 3)

    def test_missing_group_names_library_and_chunk(self):
        container(self.romfs, 'Game_0_P.utoc', [(LIB, library([(GROUP, b'missing', b'missing')]))])
        with self.assertRaisesRegex(ValueError, rf'Game_0_P.utoc: shader group 0 \({GROUP.hex()}\).*no container'):
            self.run_job()

    def test_encrypted_group_is_refused_before_data_read(self):
        raw = b'LSLG-encrypted'
        container(self.romfs, 'Game.utoc', [(GROUP, raw)], encrypted=True)
        container(self.romfs, 'Game_0_P.utoc', [(LIB, library([(GROUP, raw, raw)]))])
        with patch.object(uepak, 'aes_decrypt', side_effect=AssertionError('must not decrypt with a dummy key')) as aes:
            with self.assertRaisesRegex(ValueError, 'Game.utoc:.*encrypted.*no key'):
                self.run_job()
            aes.assert_not_called()

    def test_encrypted_library_is_detected_but_refused_on_read(self):
        container(self.romfs, 'Game.utoc', [(LIB, library([]))], encrypted=True)
        self.assertEqual(uepak.io_shader_tocs(str(self.romfs)), [str(self.romfs / 'Game.utoc')])
        with self.assertRaisesRegex(ValueError, 'Game.utoc:.*encrypted.*no key'):
            self.run_job()

    def test_superseded_encrypted_library_is_not_read(self):
        raw = b'LSLG-patch'
        container(self.romfs, 'Game.utoc', [(LIB, library([]))], encrypted=True)
        container(self.romfs, 'Game_0_P.utoc', [(LIB, library([(GROUP, raw, raw)])), (GROUP, raw)])
        entries, _ = self.run_job()
        self.assertEqual(len(entries), 1); self.assertEqual(self.shaders(entries[0]), [raw])

    def test_irrelevant_encrypted_and_ue4_containers_are_not_read(self):
        container(self.romfs, 'unrelated.utoc', [(OTHER, b'unrelated')], encrypted=True)
        container(self.romfs, 'ue4.utoc', [(LIB, b'not a UE5 library')], version=2)
        (self.romfs / 'not-a-toc.utoc').write_bytes(b'not a toc')
        self.assertEqual(uepak.io_shader_tocs(str(self.romfs)), [])
        self.assertEqual(self.run_job(), ([], 0))


if __name__ == '__main__': unittest.main()
