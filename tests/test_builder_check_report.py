"""tools/builder/check_report.py (S2.3): one source for the builder's and the game's logs, the exported report's data
(JSON ready, with the input files measured and small ones hashed) and its text form."""
import json
import os
import tempfile
import unittest
from pathlib import Path

from tools.builder import check_report as cr
from tools.builder.checks import CheckResult, Status
from tools.builder.games import GAMES

NOW = 1_700_000_000.0


def write(path, data=b'data'):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


class Logs(unittest.TestCase):
    def test_the_logs_folder_is_the_launchers(self):
        self.assertEqual(cr.logs_dir('/Users/me'), Path('/Users/me/Library/Logs/MacNativeGames'))

    def test_the_latest_builder_logs_and_the_game_logs_are_listed(self):
        with tempfile.TemporaryDirectory() as folder:
            logs = Path(folder)
            for index in range(7):
                path = write(logs / f'builder-totk-2026010{index}-120000.log', b'x' * (index + 1))
                os.utime(path, (NOW + index, NOW + index))
            write(logs / 'builder-botw-20260101-120000.log')
            write(logs / 'totk.log', b'game')
            files = cr.log_files(logs, 'totk')
        builder = [f for f in files if f.kind == 'builder']
        self.assertEqual(len(builder), cr.BUILDER_KEEP)
        self.assertEqual(builder[0].path.name, 'builder-totk-20260106-120000.log')
        game = {f.path.name: f for f in files if f.kind == 'game'}
        self.assertTrue(game['totk.log'].exists)
        self.assertFalse(game['totk.previous.log'].exists)

    def test_the_cli_lines_name_every_log_that_exists(self):
        with tempfile.TemporaryDirectory() as folder:
            logs = Path(folder)
            write(logs / 'builder-totk-20260101-120000.log')
            write(logs / 'totk.log')
            lines = cr.log_lines(logs, 'totk')
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[0].startswith('builder: '))
        self.assertTrue(lines[1].startswith('game: '))

    def test_no_log_yet_is_said_rather_than_an_empty_list(self):
        with tempfile.TemporaryDirectory() as folder:
            lines = cr.log_lines(Path(folder), 'totk')
        self.assertEqual(len(lines), 1)
        self.assertIn('(no log yet)', lines[0])


class Data(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.game = write(self.root / 'Tears.xci', b'game file')
        self.keys = write(self.root / 'prod.keys', b'header_key = 00112233445566778899aabbccddeeff\n')
        self.update = write(self.root / 'update.nsp', b'update')
        self.firmware = self.root / 'firmware'
        self.firmware.mkdir()
        self.paths = {'game': str(self.game), 'keys': str(self.keys), 'update': str(self.update),
                      'firmware': str(self.firmware), 'shared_fonts': str(self.root / 'no-fonts')}
        self.results = [CheckResult('keys', Status.WARN, 'msg.keys_missing_names', 'master_key_00'),
                        CheckResult('git', Status.OK, 'ok', '/usr/bin/git')]
        self.logs = self.root / 'logs'
        write(self.logs / 'builder-totk-20260101-120000.log', b'log')

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_data_holds_the_game_the_time_the_logs_the_inputs_and_every_check(self):
        data = cr.report_data(GAMES['totk'], self.results, self.logs, self.paths, NOW,
                              labels={'keys': 'Keys file'})
        self.assertEqual(data['format'], cr.FORMAT)
        self.assertEqual(data['game'], {'id': 'totk', 'name': 'Tears of the Kingdom',
                                        'title_id': '0100F2C0115B6000'})
        self.assertEqual(data['logs_dir'], str(self.logs))
        self.assertEqual(data['logs']['builder'][0]['path'],
                         str(self.logs / 'builder-totk-20260101-120000.log'))
        files = {item['field']: item for item in data['inputs']}
        self.assertEqual(files['game']['size'], len(b'game file'))
        self.assertEqual(len(files['game']['sha256']), 64)
        self.assertEqual(files['firmware']['kind'], 'folder')
        self.assertFalse(files['shared_fonts']['exists'])
        self.assertEqual(data['checks'][0]['label'], 'Keys file')
        self.assertEqual(data['checks'][0]['status'], 'WARN')
        self.assertEqual(data['checks'][1]['status'], 'OK')
        self.assertEqual(data['problems'], [])

    def test_only_the_fields_given_are_read(self):
        data = cr.report_data(GAMES['totk'], self.results, self.logs,
                              {'game': str(self.game)}, NOW)
        self.assertEqual(data['inputs'][0]['field'], 'game')     # only the fields given are read
        self.assertNotIn('keys', [item['field'] for item in data['inputs']])

    def test_the_text_report_names_the_fix_and_the_logs(self):
        data = cr.report_data(GAMES['totk'], self.results, self.logs, self.paths, NOW)
        body = cr.text_report(data)
        self.assertIn('Tears of the Kingdom', body)
        self.assertIn('logs:', body)
        self.assertIn('builder-totk-20260101-120000.log', body)
        self.assertIn('msg.keys_missing_names', body)
        self.assertIn('master_key_00', body)
        self.assertIn('OK', body)

    def test_the_report_writes_json_or_text_by_the_name_and_creates_the_folder(self):
        data = cr.report_data(GAMES['totk'], self.results, self.logs, self.paths, NOW)
        target = self.root / 'new/dir/report.json'
        self.assertEqual(cr.render(data, target), target)
        self.assertEqual(json.loads(target.read_text())['game']['id'], 'totk')
        text = self.root / 'report.txt'
        cr.render(data, text)
        self.assertTrue(text.read_text().startswith('MacNativeGames builder check report'))

    def test_the_suggested_path_is_in_the_logs_folder_and_named_for_the_game(self):
        path = cr.suggested_path(self.logs, 'botw', NOW)
        self.assertEqual(path.parent, self.logs)
        self.assertTrue(path.name.startswith('check-botw-'))
        self.assertEqual(path.suffix, '.json')


if __name__ == '__main__':
    unittest.main()
