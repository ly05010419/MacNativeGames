"""split_card.py: the NSP it writes for each title of a combined card reads back as the same files."""
import io
import unittest

from games.acnh.tools.extract_exefs import partition_entries
from games.acnh.tools.split_card import pfs0_header, role_of

BASE = 0x01006F8002326000


class SplitCardTests(unittest.TestCase):
    def test_pfs0_header_lists_the_files_in_order(self):
        files = [('a.cnmt.nca', 0, 3584), ('0123456789abcdef0123456789abcdef.nca', 0, 1 << 33)]
        header = pfs0_header(files)
        self.assertEqual(len(header) % 0x20, 0)
        data = header + bytes(3584) + bytes(16)   # the payload's start is enough for the table's bounds
        entries = partition_entries(io.BytesIO(data), 0, len(header) + 3584 + (1 << 33))
        self.assertEqual([(n, o - len(header), s) for n, o, s in entries],
                         [('a.cnmt.nca', 0, 3584), (files[1][0], 3584, 1 << 33)])

    def test_titles_take_their_role(self):
        self.assertEqual(role_of('01006F8002326000', BASE), 'base')
        self.assertEqual(role_of('01006F8002326800', BASE), 'update')
        self.assertEqual(role_of('01006F80023273E8', BASE), 'dlc')
        with self.assertRaises(ValueError):
            role_of('0100F2C0115B6000', BASE)


if __name__ == '__main__':
    unittest.main()


class UnsignedUpdateTests(unittest.TestCase):
    """A combined card's re-encrypted update: its signatures may fail only when the merged RomFS is pinned."""

    LOG = ('Fixed-Key Signature (FAIL):         2CDD\n'
           'NPDM Signature (FAIL):              2B83\n'
           '        Level 0 (GOOD):\n')

    def test_signature_failures_pass_only_with_a_pin(self):
        from games.acnh.tools.extract_update_romfs import merge_failures, signatures_only
        failures = merge_failures(self.LOG)
        self.assertEqual(len(failures), 2)
        self.assertTrue(signatures_only(failures, 'ab' * 32))
        self.assertFalse(signatures_only(failures, None))
        self.assertFalse(signatures_only([], 'ab' * 32))

    def test_data_failures_never_pass(self):
        from games.acnh.tools.extract_update_romfs import merge_failures, signatures_only
        for line in ('        Level 3 (FAIL):', 'Failed to read the base', 'Error: bad BKTR'):
            self.assertFalse(signatures_only(merge_failures(self.LOG + line + '\n'), 'ab' * 32))
