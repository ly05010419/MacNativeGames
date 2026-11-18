import hashlib
import json
import struct
import sys
import tempfile
import unittest
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from survey_linkage import add_nro_exports, classify, slot_address_stubs


class LinkageSurveyTests(unittest.TestCase):
    def test_plt_scan_requires_matching_slot_and_stub(self):
        adrp = 0x90000010 | (2 << 29)
        ldr = 0xf9400211 | (4 << 10)
        add = 0x91000210 | (32 << 10)
        data = struct.pack('<IIII', adrp, ldr, add, 0xd61f0220)
        self.assertEqual(slot_address_stubs(data, len(data), {0x2020, 0x2028}), {0x2020})
        broken = struct.pack('<IIII', adrp, ldr, add + (8 << 10), 0xd61f0220)
        self.assertEqual(slot_address_stubs(broken, len(broken), {0x2020}), set())
        with self.assertRaises(ValueError):
            slot_address_stubs(data, len(data) - 1, {0x2020})

    def test_candidates_and_weak_imports(self):
        imports = [('main', b'a', 1026), ('main', b'b', 1026), ('main', b'c', 1025), ('main', b'd', 1026)]
        exports = {b'a': {'sdk', 'subsdk1'}, b'b': {'sdk'}, b'd': {'main'}}
        bindings = {'main': {b'c': {2}, b'd': {1}}}
        result = classify(imports, exports, bindings)
        self.assertEqual(result['multiple_candidates'], 1)
        self.assertEqual(result['multiple_including_sdk_and_subsdk1'], 1)
        self.assertEqual(result['unique_candidate'], 1)
        self.assertEqual(result['no_candidate'], 2)
        self.assertEqual(result['unmatched_weak_only'], 1)
        self.assertEqual(result['unmatched_not_proven_weak'], 1)
        self.assertEqual(result['unmatched_not_proven_weak_kind_1026'], 1)

    def test_nro_exports_require_matching_romfs_digest(self):
        data = bytearray(0x200)
        struct.pack_into('<I', data, 4, 0x80)
        data[16:20] = b'NRO0'
        data[0x80:0x84] = b'MOD0'
        struct.pack_into('<i', data, 0x84, 0x20)
        for i, (tag, value) in enumerate(((4, 0x100), (5, 0x180), (6, 0x110), (10, 6), (0, 0))):
            struct.pack_into('<qQ', data, 0xa0 + i * 16, tag, value)
        struct.pack_into('<II', data, 0x100, 1, 2)
        struct.pack_into('<IBBHQQ', data, 0x110 + 24, 1, 0x10, 0, 1, 0x20, 0)
        data[0x180:0x186] = b'\0demo\0'
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            image = root / 'romfs.bin'
            image.write_bytes(data)
            index = {'romfs_sha256': hashlib.sha256(data).hexdigest(),
                     'files': [{'path': 'test.nro', 'offset': 0, 'size': len(data)}]}
            (root / 'index.json').write_text(json.dumps(index))
            exports = defaultdict(set)
            details = {'bindings': {}, 'relocations': {}, 'kinds': Counter(), 'unresolved': []}
            self.assertEqual(add_nro_exports(root, exports, details), {'nro_files': 1, 'nrr_files': 0,
                             'nro_digests_listed_in_nrr': 0, 'nrr_unique_digests': 0})
            self.assertEqual(exports[b'demo'], {'nro#0'})
            self.assertEqual(details['relocations'], {'nro#0': 0})
            image.write_bytes(data + b'x')
            with self.assertRaisesRegex(ValueError, 'differs'):
                add_nro_exports(root, defaultdict(set))

    def test_registered_nro_digest(self):
        nro = bytearray(0x200)
        struct.pack_into('<I', nro, 4, 0x80)
        nro[16:20], nro[0x80:0x84] = b'NRO0', b'MOD0'
        struct.pack_into('<i', nro, 0x84, 0x20)
        for i, (tag, value) in enumerate(((4, 0x100), (5, 0x180), (6, 0x110), (10, 6), (0, 0))):
            struct.pack_into('<qQ', nro, 0xa0 + i * 16, tag, value)
        struct.pack_into('<II', nro, 0x100, 1, 2)
        struct.pack_into('<IBBHQQ', nro, 0x128, 1, 0x10, 0, 1, 0x20, 0)
        nro[0x180:0x186] = b'\0demo\0'
        nrr = bytearray(0x400)
        nrr[:4] = b'NRR0'
        struct.pack_into('<II', nrr, 0x340, 0x360, 1)
        nrr[0x360:0x380] = hashlib.sha256(nro).digest()
        data = nro + nrr
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'romfs.bin').write_bytes(data)
            index = {'romfs_sha256': hashlib.sha256(data).hexdigest(), 'files': [
                {'path': 'test.nro', 'offset': 0, 'size': len(nro)},
                {'path': 'test.nrr', 'offset': len(nro), 'size': len(nrr)}]}
            (root / 'index.json').write_text(json.dumps(index))
            self.assertEqual(add_nro_exports(root, defaultdict(set))['nro_digests_listed_in_nrr'], 1)
            data[len(nro) + 0x360] ^= 1
            (root / 'romfs.bin').write_bytes(data)
            index['romfs_sha256'] = hashlib.sha256(data).hexdigest()
            (root / 'index.json').write_text(json.dumps(index))
            self.assertEqual(add_nro_exports(root, defaultdict(set))['nro_digests_listed_in_nrr'], 0)


if __name__ == '__main__':
    unittest.main()
