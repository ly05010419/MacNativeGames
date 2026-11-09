"""Reject wrong titles and failed or incomplete integrity checks, including exit-zero hactool failures."""
import importlib.util
from pathlib import Path
import tempfile
import json
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
spec = importlib.util.spec_from_file_location('xcx_merge', SCRIPTS / 'extract_update_romfs.py')
merge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(merge)


class UpdateMergeTest(unittest.TestCase):
    def run_merge(self, output_lines, title='0100453019AA8000'):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base, update, keys = (root / name for name in ('base.nsp', 'update.nsp', 'input.txt'))
            for path in (base, update, keys):
                path.write_bytes(b'')
            header = bytearray(0xc00)
            header[0x404] = 4
            base_record = dict(content_type=0, title_id=title, size=0, nca_offset=0)
            update_record = dict(base_record)
            out = root / 'merged'

            def hactool(command, **kwargs):
                self.assertIn('--suppresskeys', command)
                self.assertFalse(any(arg.startswith('--titlekey=') for arg in command))
                private = Path(kwargs['env']['MNG_TITLE_KEYS'])
                self.assertEqual(private.stat().st_mode & 0o777, 0o600)
                image = Path(next(arg.split('=', 1)[1] for arg in command if arg.startswith('--section')))
                image.write_bytes(b'synthetic RomFS; cryptography is covered by the real input test')
                kwargs['stdout'].write(output_lines)
                return SimpleNamespace(returncode=0)

            with patch.object(merge, 'outside_source', side_effect=lambda path: path.resolve()), \
                 patch.object(merge, 'load_keys', return_value={}), \
                 patch.object(merge, 'container_entries', return_value=('nsp', [])), \
                 patch.object(merge, 'common_tickets', return_value={}), \
                 patch.object(merge, 'nca_inventory', side_effect=[[(base_record, bytes(header))], [(update_record, bytes(header))]]), \
                 patch.object(merge.shutil, 'disk_usage', return_value=SimpleNamespace(free=100 * 2**30)), \
                 patch.object(merge.subprocess, 'run', side_effect=hactool), \
                 patch.object(merge, 'index_romfs', return_value=[]):
                try:
                    merge.merge(base, update, keys, root / 'hactool', out)
                    self.assertEqual(json.loads((out / 'index.json').read_text())['status'], 'pass')
                finally:
                    self.assertFalse(list(root.glob('update-merge-*')))

    def test_signature_unavailability_does_not_hide_good_data(self):
        self.run_merge('Fixed-Key Signature (FAIL):\nNPDM Signature (FAIL):\n' +
                       ''.join(f'Level {i} (GOOD)\n' for i in range(6)))

    def test_exit_zero_data_failure_still_stops(self):
        with self.assertRaisesRegex(ValueError, 'BKTR verification failed'):
            self.run_merge(''.join(f'Level {i} (GOOD)\n' for i in range(6)) + 'Level 5 (FAIL)\n')

    def test_missing_verification_level_stops(self):
        with self.assertRaisesRegex(ValueError, 'Missing verified IVFC levels'):
            self.run_merge(''.join(f'Level {i} (GOOD)\n' for i in range(5)))

    def test_other_game_stops_before_hactool(self):
        with self.assertRaisesRegex(ValueError, 'Expected base game'):
            self.run_merge('', title='0100000000000000')
