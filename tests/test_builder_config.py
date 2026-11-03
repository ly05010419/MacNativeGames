"""tools/builder/config.py (S1.6): loading and validation (C2), unknown keys at every level, the round trip.
Saving, atomicity and the broken-file backup are in test_builder_config_save.py."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.builder import config
from tools.builder.config import LastBuild, SavedPaths, Settings, SettingsStore, default_settings_path

LAST = {'game': 'totk', 'version': '1.4.3', 'app': '/A/Tears of the Kingdom.app', 'commit': 'abc123',
        'finished_at': '2026-10-23T12:00:00+11:00'}


class Clock:
    def monotonic(self):
        return 0.0

    def wall(self):
        return 0.0


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = Path(tmp.name) / 'Builder' / 'settings.json'
        self.path.parent.mkdir()
        self.store = SettingsStore(self.path, Clock())

    def write(self, content):
        self.path.write_bytes(content if isinstance(content, bytes) else json.dumps(content).encode())

    def load(self):
        try:
            return self.store.load()
        except Exception as e:          # the contract: nothing on disk makes load() raise
            self.fail(f'load() raised {e!r}')


class Missing(Base):
    def test_missing_file(self):
        result = self.load()
        self.assertEqual((result.settings, result.existed, result.problems), (Settings(), False, ()))
        self.assertFalse(self.store._broken)

    def test_default_path(self):
        self.assertEqual(default_settings_path('/Users/p'),
                         Path('/Users/p/Library/Application Support/MacNativeGames/Builder/settings.json'))


class ReadStage(Base):
    def assertBroken(self, problem):
        result = self.load()
        self.assertEqual((result.settings, result.existed, result.problems), (Settings(), True, (problem,)))
        self.assertTrue(self.store._broken)

    def test_unreadable(self):
        self.write({'format': 1})
        with patch.object(config, '_read_bytes', side_effect=PermissionError('denied')):
            self.assertBroken('unreadable')

    def test_each_cause(self):
        cases = [(b'\xff\xfe{', 'not_utf8'), (b'{"a":', 'invalid_json'), (b'{"a": ' + b'9' * 5000 + b'}', 'invalid_json'),
                 (b'[' * 200_000, 'invalid_json'), (b'[]', 'not_object')]
        for content, problem in cases:
            with self.subTest(problem=problem, content=content[:12]):
                self.write(content)
                self.assertBroken(problem)


class ContentStage(Base):
    def problems(self, data):
        self.write(data)
        result = self.load()
        self.assertFalse(self.store._broken)
        return result

    def test_an_invalid_field_drops_only_that_field(self):
        r = self.problems({'format': 1, 'games': {'totk': {'game': '/g.xci', 'keys': 7}}})
        self.assertEqual(r.problems, ('field_invalid:totk.keys',))
        self.assertEqual(r.settings.paths['totk'], SavedPaths(game='/g.xci'))

    def test_each_part(self):
        cases = [({'format': 1, 'games': []}, ('games_invalid',)),
                 ({'format': 1, 'games': {'botw': 'x'}}, ('game_invalid:botw',)),
                 ({'format': 1, 'last_build': {**LAST, 'version': 3}}, ('last_build_invalid',)),
                 ({'format': 1, 'last_build': {k: v for k, v in LAST.items() if k != 'commit'}}, ('last_build_invalid',)),
                 ({'format': 1, 'last_build': 'x'}, ('last_build_invalid',)),
                 ({'format': '1'}, ('format_invalid',)), ({'format': True}, ('format_invalid',)),
                 ({'format': 1.0}, ('format_invalid',)), ({'format': 0}, ('format_invalid',)), ({}, ('format_invalid',))]
        for data, problems in cases:
            with self.subTest(data=data):
                r = self.problems(data)
                self.assertEqual(r.problems, problems)
                self.assertEqual(r.settings.format, 1)
                self.assertIsNone(r.settings.last_build)

    def test_newer_format_is_a_warning_and_kept(self):
        r = self.problems({'format': 2, 'games': {'totk': {'game': '/g.xci'}}})
        self.assertEqual((r.problems, r.settings.format, r.settings.paths['totk'].game), (('format_newer',), 2, '/g.xci'))

    def test_several_problems_in_order(self):
        r = self.problems({'format': 'x', 'games': {'totk': {'keys': 1, 'out': 2}, 'botw': []}, 'last_build': 5})
        self.assertEqual(r.problems, ('format_invalid', 'field_invalid:totk.keys', 'field_invalid:totk.out',
                                      'game_invalid:botw', 'last_build_invalid'))


class RoundTrip(Base):
    FULL = Settings(
        paths={'totk': SavedPaths(*(f'/Users/p/Juegos – Zelda/{f}' for f in config.PATH_FIELDS)),
               'botw': SavedPaths(game='/b.xci', shared_fonts='/fonts')},
        last_build=LastBuild(**LAST))

    def test_full_settings(self):
        self.store.save(self.FULL)
        first = self.path.read_bytes()
        self.assertEqual(self.load().settings, self.FULL)
        self.store.save(self.load().settings)
        self.assertEqual(self.path.read_bytes(), first)                 # sort_keys: byte-identical
        self.assertIn('Juegos – Zelda', first.decode())                 # non-ASCII kept as written

    def test_unknown_keys_survive_at_every_level(self):
        data = {'format': 1, 'theme': 'dark', 'games': {'totk': {'game': '/g.xci', 'future_field': [1, 2]},
                                                        'third': {'anything': True}},
                'last_build': {**LAST, 'future_field': {'x': 1}}}
        self.write(data)
        self.store.save(self.load().settings)
        saved = json.loads(self.path.read_text())
        self.assertEqual((saved['theme'], saved['games']['third'], saved['games']['totk']['future_field'],
                          saved['last_build']['future_field']), ('dark', {'anything': True}, [1, 2], {'x': 1}))
        self.assertEqual(self.load().problems, ())


if __name__ == '__main__':
    unittest.main()
