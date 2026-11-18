import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import rewrite_modules as rewrite
from expected_inputs import check_supported_files, check_supported_romfs


class RewriteTests(unittest.TestCase):
    def test_missing_artifact_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            exefs = root / 'exefs'
            exefs.mkdir()
            files = []
            for name in (*rewrite.MODULES, 'main.npdm'):
                data = ('synthetic-' + name).encode()
                (exefs / name).write_bytes(data)
                files.append({'name': name, 'sha256': hashlib.sha256(data).hexdigest()})
            (root / 'extraction-manifest.json').write_text(json.dumps({
                'program': {'title_id': '0100FE701475A000'}, 'files': files}))
            out = root / 'rewritten'
            def fake_run(cmd, **kwargs):
                directory, name = Path(cmd[3]), cmd[4]
                for suffix in rewrite.OUTPUTS:
                    if not (name == 'main' and suffix == 'patched.bin'):
                        (directory / f'{name}.{suffix}').write_bytes(b'synthetic')
                if name == 'main':
                    (directory / 'main.rules.tsv').write_bytes(b'synthetic rules')
                return SimpleNamespace(returncode=0)
            with patch.object(sys, 'argv', ['rewrite_modules.py', '--exefs', str(exefs), '--out', str(out)]), \
                 patch.object(rewrite, 'check_supported_files', return_value='synthetic'), \
                 patch.object(rewrite.importlib.metadata, 'version', return_value='test'), \
                 patch.object(rewrite.subprocess, 'run', side_effect=fake_run):
                self.assertEqual(rewrite.main(), 1)
            report = json.loads((out / 'report.json').read_text())
            self.assertEqual(report['modules']['main']['exit_code'], 1)
            self.assertEqual(report['modules']['sdk']['exit_code'], 0)
            self.assertEqual(set(report['modules']['sdk']['outputs']), set(rewrite.OUTPUTS))

    def test_main_graphics_rules_are_fingerprinted(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            exefs = root / 'exefs'
            exefs.mkdir()
            files = []
            for name in (*rewrite.MODULES, 'main.npdm'):
                data = ('synthetic-' + name).encode()
                (exefs / name).write_bytes(data)
                files.append({'name': name, 'sha256': hashlib.sha256(data).hexdigest()})
            (root / 'extraction-manifest.json').write_text(json.dumps({
                'program': {'title_id': '0100FE701475A000'}, 'files': files}))
            out = root / 'rewritten'
            def fake_run(cmd, **kwargs):
                directory, name = Path(cmd[3]), cmd[4]
                self.assertEqual(kwargs['env']['TMNT_GRAPHICS_GATES'], '1')
                for suffix in rewrite.OUTPUTS:
                    (directory / f'{name}.{suffix}').write_bytes(b'synthetic')
                if name == 'main':
                    (directory / 'main.rules.tsv').write_bytes(b'synthetic rules')
                return SimpleNamespace(returncode=0)
            with patch.object(sys, 'argv', ['rewrite_modules.py', '--exefs', str(exefs), '--out', str(out)]), \
                 patch.object(rewrite, 'check_supported_files', return_value='synthetic'), \
                 patch.object(rewrite.importlib.metadata, 'version', return_value='test'), \
                 patch.object(rewrite.subprocess, 'run', side_effect=fake_run):
                self.assertEqual(rewrite.main(), 0)
            report = json.loads((out / 'report.json').read_text())
            self.assertTrue(report['options']['tmnt_graphics_gates'])
            self.assertIn('tmnt_graphics_rules.py', report['tools'])
            self.assertIn('rules.tsv', report['modules']['main']['outputs'])
            self.assertNotIn('rules.tsv', report['modules']['sdk']['outputs'])

    def test_supported_version_hash_table_fails_closed(self):
        from expected_inputs import EXPECTED
        expected = json.loads(EXPECTED.read_text())
        files = [{'name': name, 'sha256': digest} for name, digest in expected['files'].items()]
        self.assertEqual(check_supported_files(expected['title_id'], files), '1.0.0')
        files[0]['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            check_supported_files(expected['title_id'], files)
        files[0]['sha256'] = expected['files'][files[0]['name']]
        with self.assertRaisesRegex(ValueError, 'unsupported TMNT title'):
            check_supported_files('0000000000000000', files)
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            check_supported_files(expected['title_id'], files[:-1])
        self.assertEqual(check_supported_romfs(expected['romfs_sha256']), '1.0.0')
        with self.assertRaisesRegex(ValueError, 'RomFS version'):
            check_supported_romfs('0' * 64)


if __name__ == '__main__':
    unittest.main()
