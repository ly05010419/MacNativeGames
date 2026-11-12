"""Shader and pipeline contributions go only to the shader inbox (user 2026-11-12): the apps upload over HTTPS
(totk_contrib_upload.h) and games/totk/scripts/collect_shaders.py --inbox merges what arrives. The old route, players
filing GitHub issues with their shader-list-contribution*.txt, is gone: no issue form, no issue URL in any game's
contribution header or settings panel, no issue form or title prefix in any game_version.h."""
import re
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ISSUE_ROUTE = re.compile(r'issues/new|ISSUE_TEMPLATE|CONTRIB_ISSUE_URL|GAME_CONTRIB_(TEMPLATE|TITLE)|shader_contrib_watch'
                         r'|Send it in a GitHub issue')


def tracked(*patterns):
    out = subprocess.run(['git', 'ls-files', '--', *patterns], cwd=REPO, capture_output=True, text=True, check=True).stdout
    return [REPO / name for name in out.splitlines() if (REPO / name).is_file()]


class InboxOnly(unittest.TestCase):
    def test_no_issue_forms(self):
        self.assertEqual(sorted(p.name for p in (REPO / '.github/ISSUE_TEMPLATE').glob('shader-list*.yml')), [])

    def test_no_game_names_an_issue_route(self):
        files = tracked('games/*/third_party/d2r_gfx/gfx/nvn/*_shader_contrib.h', 'games/*/runtime/game_version.h',
                        'games/*/runtime/settings_panel.h', 'games/*/app/launcher.m', 'games/*/ios/tools/fill_shaders.py')
        self.assertGreaterEqual(len([f for f in files if f.name.endswith('_shader_contrib.h')]), 21)
        found = [f'{f.relative_to(REPO)}:{n}' for f in files for n, line in enumerate(f.read_text().splitlines(), 1)
                 if ISSUE_ROUTE.search(line)]
        self.assertEqual(found, [], 'contributions go to the shader inbox only')

    def test_uploading_games_name_the_inbox_in_the_file(self):
        for upload in tracked('games/*/third_party/d2r_gfx/gfx/nvn/totk_contrib_upload.h'):
            contrib = (upload.parent / 'totk_shader_contrib.h').read_text()
            self.assertIn('# This game sends it to the shader inbox', contrib, upload.relative_to(REPO))


if __name__ == '__main__':
    unittest.main()
