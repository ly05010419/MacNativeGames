"""MNG Launcher's pages besides building (tools/builder_jobs/tabs.py and tab_*.py): one JSON answer per call, the rules
of tools/builder's models, and writes only under the home given (MNG_HOME in the window's calls; a temporary one here)."""
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.builder_jobs import tabs  # noqa: E402


def fake_app(folder, bundle='org.macnativegames.totk', version='v143'):
    """An app tools/builder reads: its bundle ID and one extracted version (game_state.app_version)."""
    app = Path(folder) / 'Tears of the Kingdom.app'
    (app / 'Contents').mkdir(parents=True)
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': bundle}))
    (app / f'Contents/Resources/totk/extracted/{version}').mkdir(parents=True)
    return app


class Bridge(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.home = self.tmp / 'home'
        self.app = fake_app(self.tmp)

    def test_the_context_is_under_the_home_given(self):
        ctx = tabs.context(self.app, self.home)
        self.assertEqual(ctx.spec.id, 'totk')
        self.assertTrue(ctx.data_dir.is_relative_to(self.home) and ctx.builder_dir.is_relative_to(self.home))

    def test_other_games_and_non_apps_are_refused_in_words(self):
        other = fake_app(self.tmp / 'x', bundle='org.macnativegames.oot')
        self.assertFalse(tabs.call('settings', 'state', other, home=self.home)['ok'])
        self.assertFalse(tabs.call('settings', 'state', self.tmp / 'none.app', home=self.home)['ok'])

    def test_unknown_tabs_and_actions_say_what_exists(self):
        self.assertIn('settings', tabs.call('nothing', 'state', self.app, home=self.home)['error'])
        self.assertIn('choose', tabs.call('settings', 'nothing', self.app, home=self.home)['error'])

    def test_settings_choose_writes_the_panel_s_file_under_the_home_only(self):
        answer = tabs.call('settings', 'choose', self.app, {'option': 'smoothing', 'choice': 'on'}, home=self.home)
        self.assertTrue(answer['ok'], answer)
        self.assertEqual({o['id']: o['chosen'] for o in answer['result']['options']}['smoothing'], 'on')
        written = [p for p in self.home.rglob('*') if p.is_file()]
        self.assertEqual([p.name for p in written], ['graphics-taa.txt'])
        self.assertFalse(tabs.call('settings', 'choose', self.app, {'option': 'smoothing', 'choice': 'nope'}, home=self.home)['ok'])

    def test_settings_show_the_game_default_or_the_players_setting_never_automatic(self):
        # user 2026-11-12: what the game uses, by name; the defaults are the runtime's own (start_options.DEFAULTS)
        options = {o['id']: o for o in tabs.call('settings', 'state', self.app, home=self.home)['result']['options']}
        label = lambda o: next(c['label'] for c in options[o]['choices'] if c['id'] == options[o]['chosen'])
        self.assertEqual({o: label(o) for o in options},
                         {'picture': 'Game default: 1080p', 'interpolation': 'Game default: Off', 'smoothing': 'Game default: Off',
                          'ground': "Game default: the game's own", 'aspect': 'Game default: 16:9'})
        data = self.home / 'Library/Application Support/MacNativeGames/TOTK'
        data.mkdir(parents=True)
        for name, word in (('graphics-output.txt', '0'), ('graphics-aspect.txt', '1.7777778'), ('graphics-frame.txt', 'lock-30')):
            (data / name).write_text(word + '\n')
        answer = tabs.call('settings', 'state', self.app, home=self.home)['result']
        options = {o['id']: o for o in answer['options']}
        self.assertEqual((label('picture'), label('aspect'), label('interpolation')), ('1080p', '16:9', 'Frame lock 30 fps'))
        self.assertEqual(answer['notes'], [])                            # no "kept" notes: the value itself shows
        self.assertIn('50+50', [c['label'] for c in options['interpolation']['choices']])   # interp-3 is 50+50 (MFX_50)

    def test_the_command_line_answers_one_json_object(self):
        env = dict(os.environ, MNG_HOME=str(self.home), PYTHONPATH=str(REPO))
        done = subprocess.run([sys.executable, '-m', 'tools.builder_jobs', 'tab', 'settings', 'state', '--app', str(self.app)],
                              cwd=REPO, env=env, capture_output=True, text=True)
        answer = json.loads(done.stdout)
        self.assertTrue(answer['ok'], answer)
        self.assertEqual([o['id'] for o in answer['result']['options']], ['picture', 'interpolation', 'smoothing', 'ground', 'aspect'])


if __name__ == '__main__':
    unittest.main()
