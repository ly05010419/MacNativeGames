"""PlatinumGames' binding-table shader objects, without game data."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def object_data(stage):
    ctl, code = bytearray(0x880), bytearray(0x100)
    ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    code[:4] = split.CS_MAGIC if stage == 'cs' else split.GFX_MAGIC
    if stage != 'cs':
        struct.pack_into('<I', code, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
    binding = struct.pack('<I', 0x88) + bytes([0xff]) * 0x84
    return binding + struct.pack('<I', len(code)) + code + struct.pack('<I', len(ctl)) + ctl, bytes(ctl), bytes(code)


def archive(data):
    # One DAT member, exercising the recursive path used by the CPK scanner.
    name = b'test.pso\0'
    header = struct.pack('<4s6I', b'DAT\0', 1, 0x20, 0x24, 0x28, 0x38, 0)
    return (header.ljust(0x20, b'\0') + struct.pack('<I', 0x40) + b'pso\0'
            + struct.pack('<I', len(name)) + name).ljust(0x38, b'\0') + struct.pack('<I', len(data)) + bytes(4) + data


class BindingTableShaderTests(unittest.TestCase):
    def test_all_stages_through_dat_scan(self):
        for stage in split.STAGES:
            with self.subTest(stage=stage):
                data, ctl, code = object_data(stage)
                found = []
                split.scan_blob(archive(data), '/shader.dat', found)
                self.assertEqual(len(found), 1)
                self.assertEqual([(s, c, k) for _, s, c, k in found], [(stage, ctl, code)])
                self.assertIn('#PGSO', found[0][0])

    def test_truncated_or_unrelated_data_is_not_a_shader(self):
        data = object_data('ps')[0]
        for bad in (data[:3], data[:0x8c], data[:-1], bytes(0x1000), struct.pack('<I', 0xffffffff) + data):
            with self.subTest(size=len(bad)):
                found = []
                split.scan_blob(bad, '/unrelated', found)
                self.assertEqual(found, [])

    def test_control_and_code_lengths_must_agree(self):
        data = bytearray(object_data('vs')[0])
        struct.pack_into('<I', data, 0x8c + 0x100 + 4 + 0x704, 0x80)
        with self.assertRaisesRegex(SystemExit, 'code area length'):
            split.scan_blob(bytes(data), '/bad shader', [])


if __name__ == '__main__':
    unittest.main()
