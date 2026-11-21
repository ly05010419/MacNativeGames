"""Selection and integrity regressions, using synthetic data only."""
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import extract_exefs as ex
import game_versions
from extract_shared_fonts import copy_shared_fonts


def record(*, update=False, title=ex.BASE_TITLE, name=None, content_type=0):
    header = bytearray(0xc00)
    header[0x404] = 4 if update else 3
    return dict(title_id=title, content_type=content_type, name=name or str(update), nca_offset=0), header


def pfs_nca(name, data):
    """Small encrypted PFS0 with a real hash table; no game bytes or real keys."""
    strings = name.encode() + b'\0'
    pfs = (struct.pack('<4sIII', b'PFS0', 1, len(strings), 0) +
           struct.pack('<QQII', 0, len(data), 0, 0) + strings + data)
    hashes = b''.join(hashlib.sha256(pfs[i:i + 512]).digest() for i in range(0, len(pfs), 512))
    assert len(hashes) <= 512
    section = hashes.ljust(512, b'\0') + pfs
    section += bytes(-len(section) % 512)
    header = bytearray(0xc00)
    struct.pack_into('<II', header, 0x240, 6, 6 + len(section) // 512)
    fs = bytearray(512)
    fs[2:5] = b'\x01\x02\x03'
    fs[8:40] = hashlib.sha256(hashes).digest()
    struct.pack_into('<IIQQQQ', fs, 0x28, 512, 2, 0, len(hashes), 512, len(pfs))
    header[0x400:0x600] = fs
    header[0x280:0x2a0] = hashlib.sha256(fs).digest()
    key = bytes(16)
    cipher = Cipher(algorithms.AES(key), modes.CTR(bytes(8) + (0xc00 // 16).to_bytes(8, 'big'))).encryptor()
    return io.BytesIO(bytes(0xc00) + cipher.update(section) + cipher.finalize()), header


class ContainerSelection(unittest.TestCase):
    def test_combined_selects_by_bktr_independently_of_order(self):
        base, update = record(), record(update=True)
        dlc = record(title='0100DCA0064A7002', content_type=5)
        for records in ([base, dlc, update], [update, base, dlc]):
            self.assertIs(ex.program_nca(records, combined=True)[0], base[0])
            self.assertIs(ex.program_nca(records, update=True, combined=True)[0], update[0])

    def test_default_still_rejects_combined(self):
        for update in (False, True):
            with self.assertRaises(ValueError):
                ex.program_nca([record(), record(update=True)], update=update)

    def test_duplicate_or_missing_role_rejected(self):
        for records in ([record(), record()], [record(update=True)]):
            with self.assertRaises(ValueError):
                ex.program_nca(records, combined=True)

    def test_foreign_game_rejected_even_with_matching_program(self):
        with self.assertRaisesRegex(ValueError, 'another game'):
            ex.program_nca([record(), record(title='0100F2C0115B6000')], combined=True)

    def test_standalone_base_and_update_remain_supported(self):
        for update in (False, True):
            pair = record(update=update)
            self.assertIs(ex.program_nca([pair], update=update)[0], pair[0])


class Integrity(unittest.TestCase):
    def test_encrypted_pfs_roundtrip_and_tampering(self):
        source, header = pfs_nca('main.npdm', b'synthetic input')
        rec, _ = record()
        with patch.object(ex, 'nca_ctr_key', return_value=bytes(16)):
            self.assertEqual(ex.verified_pfs(source, rec, header, {}, {})[1], [('main.npdm', b'synthetic input')])
            raw = bytearray(source.getvalue())
            raw[0xc00 + 512 + 16] ^= 1
            with self.assertRaisesRegex(ValueError, 'block hash mismatch'):
                ex.verified_pfs(io.BytesIO(raw), rec, header, {}, {})

    def test_cnmt_identity_and_bounds(self):
        raw = bytearray(0x20)
        struct.pack_into('<QIB', raw, 0, int(ex.UPDATE_TITLE, 16), 327680, 0x81)
        rec, _ = record(update=True, title=ex.UPDATE_TITLE, content_type=1)
        with patch.object(ex, 'nca_ctr_key', return_value=bytes(16)):
            source, header = pfs_nca('Patch.cnmt', raw)
            self.assertEqual(ex.content_metadata(source, [(rec, header)], {}, {}, update=True)['version'], 327680)
            struct.pack_into('<H', raw, 0x10, 1)
            source, header = pfs_nca('Patch.cnmt', raw)
            with self.assertRaisesRegex(ValueError, 'out of bounds'):
                ex.content_metadata(source, [(rec, header)], {}, {}, update=True)
            raw[0] ^= 1
            source, header = pfs_nca('Patch.cnmt', raw)
            with self.assertRaisesRegex(ValueError, 'identity mismatch'):
                ex.content_metadata(source, [(rec, header)], {}, {}, update=True)

    def test_version_gate_rejects_modified_module_and_wrong_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / 'exefs'
            folder.mkdir()
            (folder / 'main').write_bytes(b'synthetic module')
            spec = {'display': 'test', 'cnmt_version': 123,
                    'modules': {'main': hashlib.sha256(b'synthetic module').hexdigest()}}
            with patch.dict(game_versions.VERSIONS, {'test': spec}):
                game_versions.verify_exefs(folder, 'test')
                (folder.parent / 'extraction-manifest.json').write_text(json.dumps({'content_meta': {'version': 999}}))
                with self.assertRaisesRegex(ValueError, 'CNMT version'):
                    game_versions.verify_exefs(folder, 'test')
                (folder / 'main').write_bytes(b'modified module')
                with self.assertRaisesRegex(ValueError, 'original SHA-256'):
                    game_versions.verify_exefs(folder, 'test')

    def test_incomplete_font_set_leaves_no_output(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'source'
            source.mkdir()
            (source / 'manifest.json').write_text(json.dumps({'status': 'pass', 'fonts': []}))
            output = Path(temp) / 'output'
            with self.assertRaisesRegex(ValueError, 'six shared fonts'):
                copy_shared_fonts(source, output)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
