"""A downloaded Launcher says when a newer MNG Launcher is out: its version label at the window's bottom right turns blue
("MNG Launcher 1.0.6 · 可更新到 1.0.7") and opens that release's page. A player had no way to know (1.0.3 stayed in use
while 1.0.4, 1.0.5 and 1.0.6 came out with build fixes). The release list the home page already reads for source updates
(source_updates.check, at most once in ten minutes) names the newest launcher-v* release too: no other request."""
import importlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
updates = importlib.import_module('tools.builder_app.source_updates')
SWIFT = ROOT / 'tools/builder_app/Sources/MNGBuilder'


def release(tag, draft=False):
    return {'tag_name': tag, 'draft': draft, 'html_url': f'https://github.com/owner/repo/releases/tag/{tag}', 'assets': []}


class NewestLauncher(unittest.TestCase):
    def test_the_highest_launcher_release_is_named(self):
        listing = [release('source-20261118-39be650b'), release('launcher-v1.0.9'), release('launcher-v1.0.10'),
                   release('launcher-v1.1.0', draft=True), release('launcher-vnext'), release('v2026.10.25')]
        self.assertEqual(updates.newest_launcher(listing),
                         {'version': '1.0.10', 'url': 'https://github.com/owner/repo/releases/tag/launcher-v1.0.10'})

    def test_a_list_without_one_names_none(self):
        self.assertIsNone(updates.newest_launcher([release('source-a')]))
        self.assertIsNone(updates.newest_launcher({'message': 'API rate limit exceeded'}))

    def test_the_home_page_check_records_it_for_the_window(self):
        with tempfile.TemporaryDirectory() as d:
            support = Path(d)
            conf = {'api': 'https://api.github.com/repos/owner/repo/releases?per_page=30', 'public_key': '',
                    'launcher': '1.0.6', 'bundled_time': 1000}
            listing = json.dumps([release('launcher-v1.0.7'), release('launcher-v1.0.6')]).encode()
            with mock.patch.object(updates, 'curl', return_value=listing) as curl:
                self.assertEqual(updates.check(support, conf, now=5000), ('none', None))
            curl.assert_called_once()                                            # the list alone: no other request
            state = json.loads((support / 'sources/update-check.json').read_text())
            self.assertEqual(state['launcher']['version'], '1.0.7')


class VersionLabel(unittest.TestCase):
    def test_only_a_release_older_than_the_newest_turns_blue_and_opens_its_page(self):
        model = (SWIFT / 'Model.swift').read_text()
        self.assertIn('guard Paths.isRelease', model)                          # a build from a checkout says nothing
        self.assertIn('options: .numeric) == .orderedDescending', model)        # 1.0.10 is newer than 1.0.9
        self.assertIn('newerLauncher = Self.newerLauncher(', model[model.index('func loadInstalled'):])   # after each read
        app = (SWIFT / 'App.swift').read_text()
        label = app[app.index('struct VersionLabel'):]
        self.assertIn('if let newer = model.newerLauncher', label)
        self.assertIn('NSWorkspace.shared.open(newer.url)', label)
        self.assertIn('Color.accentColor', label)
        # the label is laid over the window's content: the model has to reach it there
        self.assertLess(app.index('.overlay(alignment: .bottomTrailing) { VersionLabel() }'), app.index('.environmentObject(model)'))


if __name__ == '__main__':
    unittest.main()
