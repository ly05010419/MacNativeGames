import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from extract_exefs import TITLE, program_nca, validate_private_output
from inspect_packages import application_id


class SelectionTests(unittest.TestCase):
    def test_application_family(self):
        self.assertEqual(application_id('0100FE701475A800'), TITLE)
        self.assertEqual(application_id('0100FE701475B002'), TITLE)
        self.assertNotEqual(application_id('01005B9002312000'), TITLE)

    def test_output_must_be_new_and_private(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            validate_private_output(root / 'new')
            with self.assertRaises(ValueError):
                validate_private_output(root)
            with self.assertRaises(ValueError):
                validate_private_output(Path(__file__).resolve().parents[1] / 'private-output')

    def record(self, title, kind, patch=False):
        header = bytearray(0xc00)
        if patch:
            header[0x404] = 4
        return {'title_id': title, 'content_type': kind}, header

    def test_matching_base(self):
        records = [self.record(TITLE, 1), self.record(TITLE, 0)]
        self.assertEqual(program_nca(records)[0]['title_id'], TITLE)

    def test_rejects_mixed_patch_and_unrelated(self):
        for records in ([self.record(TITLE, 1), self.record(TITLE, 0, True)],
                        [self.record(TITLE, 1), self.record(TITLE, 0), self.record('0100000000000000', 2)],
                        [self.record('0100000000000000', 1), self.record('0100000000000000', 0)]):
            with self.assertRaises(ValueError):
                program_nca(records)


if __name__ == '__main__':
    unittest.main()
