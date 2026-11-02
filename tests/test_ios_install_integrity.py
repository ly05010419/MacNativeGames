"""Offline iOS image-transfer regressions; all device commands are mocked and data is synthetic."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'games/totk/ios/tools/install_device.py'
spec = importlib.util.spec_from_file_location('totk_install_device_under_test', SOURCE)
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class OfflineTest(unittest.TestCase):
    def setUp(self):
        # A test that accidentally reaches an unmocked signing/device/network command fails immediately.
        self.external = mock.patch.object(installer.subprocess, 'run',
                                          side_effect=AssertionError('Unexpected external command'))
        self.external.start()
        self.addCleanup(self.external.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)


class ImageTransferTests(OfflineTest):
    def setUp(self):
        super().setUp()
        self.payload = b'abcdefghij'
        self.image = self.folder / 'romfs.bin'
        self.image.write_bytes(self.payload)
        self.sha = hashlib.sha256(self.payload).hexdigest()
        (self.folder / 'index.json').write_text(json.dumps({'romfs_sha256': self.sha}))
        self.part_patch = mock.patch.object(installer, 'PART', 4)
        self.part_patch.start()
        self.addCleanup(self.part_patch.stop)
        self.remote = {f'romfs.bin.{i:03d}': self.payload[i * 4:(i + 1) * 4] for i in range(3)}
        self.writes = []
        self.fail_marker_read = False
        self.device_patch = mock.patch.object(installer, 'device_files',
                                              side_effect=lambda *args: {k: len(v) for k, v in self.remote.items()})
        self.device_patch.start()
        self.addCleanup(self.device_patch.stop)
        installer.subprocess.run.side_effect = self.device_copy

    def device_copy(self, command, **kwargs):
        self.assertIn('copy', command)
        direction = command[command.index('copy') + 1]
        source = command[command.index('--source') + 1]
        destination = command[command.index('--destination') + 1]
        if direction == 'from':
            name = Path(source).name
            if self.fail_marker_read:
                return subprocess.CompletedProcess(command, 1, stdout=b'', stderr=b'phone locked')
            self.assertEqual(name, 'romfs.sha256')
            Path(destination).write_bytes(self.remote[name])
        else:
            self.assertEqual(direction, 'to')
            name = Path(destination).name
            data = Path(source).read_bytes()
            self.writes.append((name, data))
            self.remote[name] = data
        return subprocess.CompletedProcess(command, 0, stdout=b'', stderr=b'')

    def push(self):
        with contextlib.redirect_stdout(io.StringIO()):
            installer.push_game_data('mock-device', 'mock.bundle', self.image)

    def assert_complete(self):
        actual = b''.join(self.remote[f'romfs.bin.{i:03d}'] for i in range(3))
        self.assertEqual(actual, self.payload)
        self.assertEqual(self.remote['romfs.sha256'], (self.sha + '\n').encode())

    def written_parts(self):
        return [name for name, _ in self.writes if name.startswith('romfs.bin.')]

    def test_current_committed_image_skips_without_writes(self):
        self.remote['romfs.sha256'] = (self.sha + '\n').encode()
        self.push()
        self.assertEqual(self.writes, [])

    def test_committed_marker_does_not_hide_truncated_part(self):
        self.remote['romfs.sha256'] = (self.sha + '\n').encode()
        self.remote['romfs.bin.001'] = b'e'
        self.push()
        self.assertEqual(self.written_parts(), ['romfs.bin.001'])
        self.assertEqual(self.writes[0], ('romfs.sha256', ('copying ' + self.sha + '\n').encode()))
        self.assert_complete()

    def test_missing_marker_cannot_commit_equal_sized_foreign_parts(self):
        self.remote = {name: b'X' * len(data) for name, data in self.remote.items()}
        self.push()
        self.assertEqual(self.written_parts(), ['romfs.bin.000', 'romfs.bin.001', 'romfs.bin.002'])
        self.assert_complete()

    def test_unknown_marker_cannot_commit_equal_sized_foreign_parts(self):
        self.remote = {name: b'X' * len(data) for name, data in self.remote.items()}
        self.remote['romfs.sha256'] = b'stale\n'
        self.push()
        self.assertEqual(len(self.written_parts()), 3)
        self.assert_complete()

    def test_foreign_copying_marker_cannot_resume_or_commit_parts(self):
        self.remote = {name: b'X' * len(data) for name, data in self.remote.items()}
        self.remote['romfs.sha256'] = ('copying ' + '0' * 64 + '\n').encode()
        self.push()
        self.assertEqual(len(self.written_parts()), 3)
        self.assert_complete()

    def test_foreign_committed_marker_cannot_resume_parts(self):
        self.remote = {name: b'X' * len(data) for name, data in self.remote.items()}
        self.remote['romfs.sha256'] = ('0' * 64 + '\n').encode()
        self.push()
        self.assertEqual(len(self.written_parts()), 3)
        self.assert_complete()

    def test_same_sha_resume_skips_only_exact_complete_parts(self):
        self.remote['romfs.sha256'] = ('copying ' + self.sha + '\n').encode()
        self.remote['romfs.bin.001'] = b'e'
        del self.remote['romfs.bin.002']
        self.push()
        self.assertEqual(self.written_parts(), ['romfs.bin.001', 'romfs.bin.002'])
        self.assert_complete()

    def test_same_sha_complete_resume_only_commits_its_marker(self):
        self.remote['romfs.sha256'] = ('copying ' + self.sha + '\n').encode()
        self.push()
        self.assertEqual(self.writes, [('romfs.sha256', (self.sha + '\n').encode())])

    def test_marker_read_failure_stops_before_any_write(self):
        self.remote['romfs.sha256'] = (self.sha + '\n').encode()
        self.fail_marker_read = True
        with self.assertRaises(SystemExit):
            self.push()
        self.assertEqual(self.writes, [])


class DeviceListingTests(OfflineTest):
    def use_responses(self, responses):
        self.queries = []
        self.commands = []

        def device_info(command, **kwargs):
            self.commands.append(command)
            sub = command[command.index('--subdirectory') + 1]
            self.queries.append(sub)
            response = responses[sub]
            if response is None:
                return subprocess.CompletedProcess(command, 1, stdout=b'', stderr=b'not readable')
            output = command[command.index('--json-output') + 1]
            Path(output).write_text(json.dumps(response))
            return subprocess.CompletedProcess(command, 0, stdout=b'', stderr=b'')

        installer.subprocess.run.side_effect = device_info

    @staticmethod
    def listing(*names):
        return {'result': {'files': [{'relativePath': name, 'metadata': {'size': 0}} for name in names]}}

    def test_existing_unreadable_child_is_not_an_empty_folder(self):
        self.use_responses({'Documents/GameData': None, 'Documents': self.listing('GameData', 'save')})
        with self.assertRaises(SystemExit):
            installer.device_files('mock', 'bundle', 'Documents/GameData')
        self.assertTrue(all('--no-recurse' in command for command in self.commands))

    def test_exact_absence_in_parent_is_an_empty_result(self):
        self.use_responses({'Documents/GameData': None, 'Documents': self.listing('save')})
        self.assertEqual(installer.device_files('mock', 'bundle', 'Documents/GameData'), {})
        self.assertEqual(self.queries, ['Documents/GameData', 'Documents'])

    def test_nested_absence_is_checked_against_immediate_parent(self):
        self.use_responses({'Documents/GameData/missing-seed': None,
                            'Documents/GameData': self.listing('romfs.sha256', 'romfs.bin.000')})
        self.assertEqual(installer.device_files('mock', 'bundle', 'Documents/GameData/missing-seed'), {})
        self.assertEqual(self.queries, ['Documents/GameData/missing-seed', 'Documents/GameData'])

    def test_unreadable_documents_cannot_prove_absence(self):
        self.use_responses({'Documents/GameData': None, 'Documents': None})
        with self.assertRaises(SystemExit):
            installer.device_files('mock', 'bundle', 'Documents/GameData')

    def test_missing_files_field_is_not_an_empty_listing(self):
        self.use_responses({'Documents': {'result': {}}})
        with self.assertRaises(SystemExit):
            installer.device_files('mock', 'bundle', 'Documents')

    def test_error_payload_cannot_prove_an_empty_listing(self):
        self.use_responses({'Documents': {'result': {'files': []}, 'error': {'code': 'mock-error'}}})
        with self.assertRaises(SystemExit):
            installer.device_files('mock', 'bundle', 'Documents')


if __name__ == '__main__':
    unittest.main()
