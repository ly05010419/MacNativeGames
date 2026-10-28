"""tools/builder/games.py: the catalog, and its drift against build_app.py's source (S1.1a, C1 and C4)."""
from pathlib import Path
import unittest

from builder_buildapp_source import SourceLayoutChanged, differences, extract
from tools.builder.games import GAMES

REPO = Path(__file__).resolve().parents[1]

# A small build_app.py with the same shapes as the real one; each test below breaks one association.
SYNTHETIC = """
REPO = Path(__file__).resolve().parent
GAME = REPO / 'games/totk'
EXPECTED = json.loads((GAME / 'assets/expected-v1.0.0.json').read_text())
UPDATED = json.loads((GAME / 'assets/expected-v1.1.2.json').read_text())
UPDATED121 = json.loads((GAME / 'assets/expected-v1.2.1.json').read_text())
UPDATED143 = json.loads((GAME / 'assets/expected-v1.4.3.json').read_text())
VERSIONS = {'v0': EXPECTED, 'v112': UPDATED, 'v121': UPDATED121, 'v143': UPDATED143}
BOTW = REPO / 'games/botw'
BOTW160 = json.loads((BOTW / 'assets/expected-v1.6.0.json').read_text())
BOTW190 = json.loads((BOTW / 'assets/expected-v1.9.0.json').read_text())
VERSIONS['botw160'] = BOTW160
VERSIONS['botw190'] = BOTW190
TITLES = {'0100F2C0115B6000': 'totk', '01007EF00011E000': 'botw'}
APPS = {'totk': ('Tears of the Kingdom', 'org.macnativegames.totk', 'TOTK'), 'botw': ('Breath of the Wild', 'org.macnativegames.botw', 'BotW')}
"""


def synthetic(old, new):
    assert old in SYNTHETIC, old
    return SYNTHETIC.replace(old, new)


class Catalog(unittest.TestCase):
    def test_two_games_with_their_versions_in_order(self):
        self.assertEqual(list(GAMES), ['totk', 'botw'])
        self.assertEqual([v.display for v in GAMES['totk'].versions], ['1.0.0', '1.1.2', '1.2.1', '1.4.3'])
        self.assertEqual([v.display for v in GAMES['botw'].versions], ['1.6.0', '1.9.0'])

    def test_every_manifest_is_named_after_its_version(self):
        for spec in GAMES.values():
            for v in spec.versions:
                self.assertEqual(v.manifest, f'expected-v{v.display}.json')


class DriftAgainstBuildApp(unittest.TestCase):
    def test_real_build_app_matches_the_catalog(self):
        facts = extract((REPO / 'build_app.py').read_text())
        self.assertEqual(differences(facts, GAMES), [])

    def test_the_synthetic_source_matches_the_catalog(self):
        self.assertEqual(differences(extract(SYNTHETIC), GAMES), [])

    def assertDrift(self, source, expected_line):
        found = differences(extract(source), GAMES)
        self.assertTrue(any(expected_line in line for line in found), found)

    def test_version_moved_to_the_other_game(self):
        self.assertDrift(synthetic("UPDATED143 = json.loads((GAME /", "UPDATED143 = json.loads((BOTW /")
                         .replace("BOTW = REPO / 'games/botw'\n", "")
                         .replace("GAME = REPO / 'games/totk'\n", "GAME = REPO / 'games/totk'\nBOTW = REPO / 'games/botw'\n"),
                         "('botw', 'v143', 'games/botw/assets/expected-v1.4.3.json')")

    def test_two_keys_swapped(self):
        self.assertDrift(synthetic("{'v0': EXPECTED, 'v112': UPDATED,", "{'v0': UPDATED, 'v112': EXPECTED,"),
                         "('totk', 'v0', 'games/totk/assets/expected-v1.1.2.json')")

    def test_key_added_upstream_and_missing_from_the_catalog(self):
        source = synthetic("VERSIONS['botw160'] = BOTW160",
                           "VERSIONS['botw160'] = BOTW160\n"
                           "UPDATED150 = json.loads((GAME / 'assets/expected-v1.5.0.json').read_text())\n"
                           "VERSIONS['v150'] = UPDATED150")
        self.assertDrift(source, "only in build_app.py: ('totk', 'v150'")

    def test_manifest_loaded_but_not_registered(self):
        source = synthetic("VERSIONS['botw160'] = BOTW160",
                           "VERSIONS['botw160'] = BOTW160\n"
                           "UPDATED150 = json.loads((GAME / 'assets/expected-v1.5.0.json').read_text())")
        self.assertDrift(source, 'loaded but not in VERSIONS: games/totk/assets/expected-v1.5.0.json')

    def test_manifest_named_but_never_loaded(self):
        source = SYNTHETIC + "def step(botw):\n    run(['x', botw / 'assets/expected-v1.7.0.json'])\n"
        self.assertDrift(source, 'named but never loaded: expected-v1.7.0.json')

    def test_title_points_to_the_wrong_game(self):
        self.assertDrift(synthetic("'01007EF00011E000': 'botw'", "'01007EF00011E000': 'totk'"), 'TITLES')

    def test_app_data_folder_changed(self):
        self.assertDrift(synthetic("'org.macnativegames.botw', 'BotW')", "'org.macnativegames.botw', 'BOTW')"), 'APPS')

    def test_source_without_versions_registry_is_a_layout_change(self):
        source = synthetic("VERSIONS = {'v0': EXPECTED, 'v112': UPDATED, 'v121': UPDATED121, 'v143': UPDATED143}", "") \
            .replace("VERSIONS['botw160'] = BOTW160", "").replace("VERSIONS['botw190'] = BOTW190", "")
        with self.assertRaisesRegex(SourceLayoutChanged, 'VERSIONS'):
            extract(source)

    def test_registry_naming_something_that_is_not_a_load_is_a_layout_change(self):
        with self.assertRaisesRegex(SourceLayoutChanged, 'not manifest loads'):
            extract(synthetic("VERSIONS['botw160'] = BOTW160", "VERSIONS['botw160'] = BOTW_LIST"))


if __name__ == '__main__':
    unittest.main()
