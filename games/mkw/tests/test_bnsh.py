import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('extract_bnsh', Path(__file__).resolve().parents[1] / 'tools/extract_bnsh.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BnshTests(unittest.TestCase):
    def fixture(self):
        data = bytearray(0x500)
        data[:8] = b'BNSH\0\0\0\0'
        struct.pack_into('<IQ', data, 0x7c, 1, 0x100)
        struct.pack_into('<Q', data, 0x110, 0x180)
        struct.pack_into('<Q', data, 0x188, 0x200)
        struct.pack_into('<QQII', data, 0x208, 0x280, 0x400, 0x100, 0xd8)
        data[0x280:0x288] = module.MAGIC
        struct.pack_into('<I', data, 0x2b8, 0x80)
        struct.pack_into('<II', data, 0x2c0, 0x100, 0x100)
        return data

    def test_stage_and_constant_area(self):
        rows = list(module.stages(self.fixture()))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][:3], (0, 0, 0))
        self.assertEqual(len(rows[0][3]), 0xd8)
        self.assertEqual(len(rows[0][4]), 0x100)

    def test_bad_descriptor_is_rejected(self):
        data = self.fixture()
        struct.pack_into('<Q', data, 0x188, 0xffffffffffffffff)
        with self.assertRaises(ValueError):
            list(module.stages(data))

    def test_truncated_code_is_rejected(self):
        with self.assertRaises(ValueError):
            list(module.stages(self.fixture()[:-1]))

    def test_lengths_must_match_descriptor(self):
        data = self.fixture()
        struct.pack_into('<I', data, 0x2c4, 0x80)
        with self.assertRaises(ValueError):
            list(module.stages(data))

    def test_bahs_preserves_mesh_stage(self):
        data = bytearray(0x500)
        data[:4] = b'BAHS'
        struct.pack_into('<II', data, 0x18, 0x100, 0x400)
        struct.pack_into('<IIIi', data, 0x20, 0, 0, 0xd8, 0x60)
        data[0x80:0x88] = module.MAGIC
        struct.pack_into('<I', data, 0xb8, 0x80)
        struct.pack_into('<II', data, 0xc0, 0x100, 0x100)
        data[0xd6] = 7
        rows = list(module.bahs_stages(data))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][2], 7)
        self.assertEqual(len(rows[0][4]), 0x100)

    def test_bahs_outside_code_region(self):
        data = bytearray(0x40); data[:4] = b'BAHS'
        struct.pack_into('<II', data, 0x18, 0x100, 0x400)
        with self.assertRaises(ValueError):
            list(module.bahs_stages(data))

    def test_sarc_invalid_file_table(self):
        data = bytearray(0x40); data[:4] = b'SARC'
        data[6:8] = b'\xff\xfe'
        struct.pack_into('<H', data, 4, 0xff00)
        with self.assertRaises(ValueError):
            list(module.sarc_members(data))


if __name__ == '__main__':
    unittest.main()
