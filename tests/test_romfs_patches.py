"""Guarded binary edits made by make_romfs_patches.py, using synthetic layouts only."""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
from make_romfs_patches import common_patch_members, patch_prompt_diagram  # noqa: E402


MEMBER = 'blyt/PaSystemController_00.bflyt'


def usd1(entries):
    """Build a usd1 section. entries are (name, type, count, encoded data)."""
    table_size = 12 + 12 * len(entries)
    data = bytearray()
    locations = []
    for name, kind, count, value in entries:
        while (table_size + len(data)) % 4:
            data += b'\0'
        value_at = table_size + len(data)
        data += value
        locations.append([name, kind, count, value_at, None])
    for location in locations:
        name = location[0].encode() + b'\0'
        location[4] = table_size + len(data)
        data += name
    data += b'\0' * (-len(data) % 4)
    out = bytearray(b'usd1' + struct.pack('<IHH', table_size + len(data), len(entries), 0))
    for i, (_, kind, count, value_at, name_at) in enumerate(locations):
        entry_at = 12 + i * 12
        out += struct.pack('<IIHBB', name_at - entry_at, value_at - entry_at, count, kind, 0)
    out += data
    return bytes(out)


def pane(name):
    return b'pan1' + struct.pack('<I', 0x54) + b'\x03\0\xff\0' + name.encode().ljust(24, b'\0') + b'\0' * (0x54 - 36)


def layout(entries, pane_name='N_CapGuide_00'):
    sections = [pane('RootPane'), pane(pane_name), usd1(entries)]
    body = b''.join(sections)
    header = b'FLYT\xff\xfe' + struct.pack('<HIIHH', 0x14, 0x09000000, 0x14 + len(body), len(sections), 0)
    return bytearray(header + body)


def active_entries(data):
    """Read active usd1 entries as (name, type, length, value, name address, data address)."""
    at = data.index(b'usd1')
    count = struct.unpack_from('<H', data, at + 8)[0]
    entries = []
    for i in range(count):
        entry = at + 12 + 12 * i
        name_offset, data_offset, length, kind, _ = struct.unpack_from('<IIHBB', data, entry)
        name, value = entry + name_offset, entry + data_offset
        entries.append((bytes(data[name:data.index(0, name)]).decode(), kind, length,
                        bytes(data[value:value + length]), name, value))
    return entries


def entry_names(data):
    return [entry[0] for entry in active_entries(data)]


class PromptDiagramPatch(unittest.TestCase):
    def entries(self, dynamic=('DynamicCaptureOn', 0, 5, b'RGBA8\0')):
        return [dynamic, ('CaptureBGColor', 1, 3, struct.pack('<III', 255, 255, 255))]

    def patch(self, data):
        before = bytes(data)
        result = patch_prompt_diagram(data, {MEMBER: (0, len(data))})
        return before, result

    def test_removes_only_dynamic_capture_entry_without_resizing(self):
        data = layout(self.entries())
        size = len(data)
        before, result = self.patch(data)
        self.assertEqual(result, [MEMBER])
        self.assertEqual(len(data), size)
        self.assertEqual(entry_names(data), ['CaptureBGColor'])
        self.assertIn(b'DynamicCaptureOn\0', data)  # stale payload is inert; the member is not resized
        self.assertNotEqual(bytes(data), before)
        before_bg = next(entry for entry in active_entries(before) if entry[0] == 'CaptureBGColor')
        self.assertEqual(active_entries(data)[0], before_bg)  # type, length, value and absolute pointers are unchanged
        usd = before.index(b'usd1')
        changed = {i for i, (old, new) in enumerate(zip(before, data)) if old != new}
        allowed = {usd + 8, usd + 9} | set(range(usd + 12, usd + 24))
        self.assertTrue(changed)
        self.assertLessEqual(changed, allowed)

    def test_removes_dynamic_capture_when_it_is_the_last_entry(self):
        entries = self.entries()
        data = layout([entries[1], entries[0]])
        before, _ = self.patch(data)
        self.assertEqual(active_entries(data), [active_entries(before)[0]])

    def test_removes_dynamic_capture_when_it_is_a_middle_entry(self):
        entries = self.entries()
        other = ('OtherString', 0, 4, b'Test\0')
        data = layout([entries[1], entries[0], other])
        before, _ = self.patch(data)
        expected = [entry for entry in active_entries(before) if entry[0] != 'DynamicCaptureOn']
        self.assertEqual(active_entries(data), expected)

    def test_rejects_missing_target_pane(self):
        data = layout(self.entries(), pane_name='N_Other_00')
        with self.assertRaisesRegex(ValueError, 'N_CapGuide_00.*not found'):
            self.patch(data)

    def test_rejects_pane_section_too_small_for_its_name(self):
        data = layout(self.entries())
        target = data.index(b'pan1', 0x14 + 0x54)
        struct.pack_into('<I', data, target + 4, 20)
        with self.assertRaisesRegex(ValueError, 'pane section.*too small'):
            self.patch(data)

    def test_rejects_truncated_layout_header(self):
        data = bytearray(b'FLYT\xff\xfe')
        with self.assertRaisesRegex(ValueError, 'truncated BFLYT header'):
            self.patch(data)

    def test_rejects_wrong_byte_order(self):
        data = layout(self.entries())
        data[4:6] = b'\xfe\xff'
        with self.assertRaisesRegex(ValueError, 'little-endian BFLYT'):
            self.patch(data)

    def test_rejects_header_offset_outside_the_file(self):
        data = layout(self.entries())
        struct.pack_into('<H', data, 6, len(data) + 1)
        with self.assertRaisesRegex(ValueError, 'header size.*outside'):
            self.patch(data)

    def test_rejects_declared_file_size_mismatch(self):
        data = layout(self.entries())
        struct.pack_into('<I', data, 12, len(data) - 4)
        with self.assertRaisesRegex(ValueError, 'declared size.*member size'):
            self.patch(data)

    def test_rejects_invalid_pane_name_encoding(self):
        data = layout(self.entries())
        target = data.index(b'N_CapGuide_00')
        data[target] = 0xff
        with self.assertRaisesRegex(ValueError, 'invalid pane name'):
            self.patch(data)

    def test_rejects_missing_dynamic_capture_entry(self):
        data = layout(self.entries()[1:])
        with self.assertRaisesRegex(ValueError, 'DynamicCaptureOn.*found 0'):
            self.patch(data)

    def test_rejects_duplicate_dynamic_capture_entries(self):
        dynamic = self.entries()[0]
        data = layout([dynamic, dynamic, self.entries()[1]])
        with self.assertRaisesRegex(ValueError, 'DynamicCaptureOn.*found 2'):
            self.patch(data)

    def test_rejects_unexpected_type(self):
        data = layout(self.entries(('DynamicCaptureOn', 1, 1, struct.pack('<I', 1))))
        with self.assertRaisesRegex(ValueError, 'type 1.*expected string'):
            self.patch(data)

    def test_rejects_unexpected_value(self):
        data = layout(self.entries(('DynamicCaptureOn', 0, 6, b'RGBA16\0')))
        with self.assertRaisesRegex(ValueError, "value 'RGBA16'.*expected 'RGBA8'"):
            self.patch(data)

    def test_only_v143_adds_the_prompt_diagram_to_common_patches(self):
        for version in ('v0', 'v121'):
            with self.subTest(version=version):
                self.assertNotIn(MEMBER, common_patch_members(version))
        self.assertIn(MEMBER, common_patch_members('v143'))


if __name__ == '__main__':
    unittest.main()
