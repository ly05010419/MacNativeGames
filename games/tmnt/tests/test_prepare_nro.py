import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from prepare_nro import nso_from_nro, prepare


def sample_nro():
    data = bytearray(0x200)
    data[0x10:0x14] = b'NRO0'
    struct.pack_into('<I', data, 0x18, len(data))
    for i, pair in enumerate(((0, 0x80), (0x80, 0x80), (0x100, 0x100))):
        struct.pack_into('<II', data, 0x20 + i * 8, *pair)
    struct.pack_into('<I', data, 0x38, 0x4000)
    return data


class NroPreparationTests(unittest.TestCase):
    def test_conversion_keeps_segments_and_rejects_gap(self):
        nro = sample_nro()
        nso = nso_from_nro(nro)
        self.assertEqual(nso[:4], b'NSO0')
        self.assertEqual(nso[0x100:], nro)
        self.assertEqual(struct.unpack_from('<III', nso, 0x20), (0x180, 0x80, 0x80))
        struct.pack_into('<I', nro, 0x28, 0x84)
        with self.assertRaisesRegex(ValueError, 'layout'):
            nso_from_nro(nro)

    def test_only_registered_module_is_prepared(self):
        nro = sample_nro()
        nrr = bytearray(0x400)
        nrr[:4] = b'NRR0'
        struct.pack_into('<II', nrr, 0x340, 0x360, 1)
        nrr[0x360:0x380] = hashlib.sha256(nro).digest()
        data = nro + nrr
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            romfs = root / 'romfs'
            romfs.mkdir()
            (romfs / 'romfs.bin').write_bytes(data)
            (romfs / 'index.json').write_text(json.dumps({'status': 'pass',
                'romfs_sha256': hashlib.sha256(data).hexdigest(), 'files': [
                    {'path': 'test.nro', 'offset': 0, 'size': len(nro)},
                    {'path': 'test.nrr', 'offset': len(nro), 'size': len(nrr)}]}))
            out = root / 'out'
            self.assertEqual(prepare(romfs, out), 1)
            self.assertEqual((out / 'module.nro').read_bytes(), nro)
            self.assertEqual((out / 'synthetic.nso').read_bytes()[0x100:], nro)
            with self.assertRaises(ValueError):
                prepare(romfs, out)
            data[-0xa0] ^= 1
            (romfs / 'romfs.bin').write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'differs'):
                prepare(romfs, root / 'other')


if __name__ == '__main__':
    unittest.main()
