"""MNG Launcher's Mods page (tools/builder_jobs/tab_mods.py over tools/builder's mod models): every function of the old
window's Mods tab through the bridge, against a fake built app, with the real installer for Apply.

HOME points at a folder the test never expects to be written, and the bridge gets another home (as MNG_HOME does): the
game's mod folder, the library and the lock must all land under the bridge's home, and HOME must come back unchanged."""
import fcntl
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / 'tests'))
from test_mod_manifest import fake_romfs  # noqa: E402
from tools.builder_jobs import tab_mods, tabs  # noqa: E402

FILES = {'Pack/A.pack.zs': b'original a', 'Sound/B.bwav': b'original b' * 100, 'UI/C.bfres': b'c'}


def fake_app(folder, version='v143', bundle='org.macnativegames.totk'):
    app = Path(folder) / 'Tears of the Kingdom.app'
    (app / 'Contents').mkdir(parents=True)
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': bundle}))
    fake_romfs(app / f'Contents/Resources/totk/extracted/{version}/romfs', FILES)
    return app


class Page(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.home = self.tmp / 'home'
        self.elsewhere = self.tmp / 'elsewhere'
        env = patch.dict(os.environ, HOME=str(self.elsewhere), MNG_LANG='en')
        env.start()
        self.addCleanup(env.stop)
        self.app = fake_app(self.tmp)
        self.load = self.home / 'Library/Application Support/MacNativeGames/TOTK/mods/v143'

    def call(self, action, app=None, **args):
        answer = tabs.call('mods', action, app or self.app, args, home=self.home)
        self.assertEqual(os.environ['HOME'], str(self.elsewhere), 'HOME must come back as it was')
        return answer

    def ok(self, action, **args):
        answer = self.call(action, **args)
        self.assertTrue(answer['ok'], answer)
        return answer['result']

    def source(self, name, files, part='romfs'):
        root = self.tmp / 'mods' / name
        for path, data in files.items():
            (root / part / path).parent.mkdir(parents=True, exist_ok=True)
            (root / part / path).write_bytes(data)
        return root

    def add(self, name, files, part='romfs'):
        return self.ok('add', path=str(self.source(name, files, part)))

    def test_an_empty_library_says_how_to_add(self):
        state = self.ok('state')
        self.assertEqual((state['reason'], state['mods'], state['version']), ('', [], '1.4.3'))
        self.assertIn('Add a mod as it came', state['empty'])
        self.assertEqual(state['labels']['apply'], 'Apply mods')

    def test_add_enable_apply_puts_the_mod_into_the_game_s_folder_under_the_home_given(self):
        added = self.add('Better Grass', {'Pack/A.pack.zs': b'modded a'})
        self.assertEqual(added['status'], {'ok': True, 'text': 'Added.'})
        [mod] = added['mods']
        self.assertEqual((mod['name'], mod['valid'], mod['enabled'], mod['code'], mod['bad']),
                         ('Better Grass', True, False, None, False))
        self.assertEqual(mod['details'], ['Valid: replaces 1 files, adds 0.'])
        self.assertTrue(self.ok('enable', mod=mod['id'], on=True)['mods'][0]['enabled'])
        self.assertIn('Apply mods again', self.ok('state')['not_applied'])
        applied = self.ok('apply')
        self.assertEqual((applied['ok'], applied['message'], applied['state']['not_applied']),
                         (True, 'Mods applied correctly.', ''))
        self.assertEqual((self.load / 'Pack/A.pack.zs').read_bytes(), b'modded a')
        self.assertTrue((self.load / 'manifest.tsv').is_file())
        self.assertFalse(self.elsewhere.exists(), 'nothing may be written outside the home given')

    def test_nothing_on_sets_the_folder_aside(self):
        self.add('A', {'Pack/A.pack.zs': b'x'})
        applied = self.ok('apply')
        self.assertEqual((applied['ok'], applied['message']),
                         (True, 'No mod is on: the mod folder was set aside, the game starts without mods.'))

    def test_a_refused_add_says_why_and_adds_nothing(self):
        (self.tmp / 'Empty').mkdir()
        added = self.ok('add', path=str(self.tmp / 'Empty'))
        self.assertFalse(added['status']['ok'])
        self.assertTrue(added['status']['text'].startswith('Not added: no romfs/ or exefs/'), added['status'])
        self.assertEqual(added['mods'], [])
        self.assertFalse(self.call('add', path=str(self.tmp / 'missing'))['ok'])

    def test_an_invalid_mod_is_listed_with_its_reason_and_cannot_be_switched_on(self):
        mod = self.add('Adds', {'Pack/New.pack.zs': b'new'})['mods'][0]
        self.assertEqual((mod['valid'], mod['bad']), (False, True))
        self.assertIn('it only adds files', mod['details'][0])
        refused = self.call('enable', mod=mod['id'], on=True)
        self.assertFalse(refused['ok'])
        self.assertIn('not valid', refused['error'])

    def test_order_moves_and_conflicts_name_the_winner(self):
        a = self.add('A', {'Pack/A.pack.zs': b'from a'})['mods'][0]['id']
        b = self.add('B', {'Pack/A.pack.zs': b'from b'})['mods'][-1]['id']
        self.ok('enable', mod=a, on=True)
        state = self.ok('enable', mod=b, on=True)
        self.assertIn('the one lower in the list wins: Pack/A.pack.zs', state['mods'][0]['details'][-1])
        moved = self.ok('move', mod=b, step=-1)
        self.assertEqual([m['id'] for m in moved['mods']], [b, a])
        self.assertFalse(self.call('move', mod=b, step=2)['ok'])
        self.ok('apply')
        self.assertEqual((self.load / 'Pack/A.pack.zs').read_bytes(), b'from a')     # the lower one wins

    def test_remove_deletes_only_the_library_s_copy(self):
        added = self.add('A', {'Pack/A.pack.zs': b'x'})
        mod = added['mods'][0]
        self.assertEqual(mod['confirm_remove'], 'Remove "A" from the list? Only the builder\'s copy is deleted; your '
                                                'folder stays.')
        self.assertEqual(self.ok('remove', mod=mod['id'])['mods'], [])
        self.assertTrue((self.tmp / 'mods/A/romfs/Pack/A.pack.zs').is_file())
        self.assertIn('no mod', self.call('remove', mod=mod['id'])['error'])

    def test_code_patches_have_their_own_toggle_and_say_the_next_full_build_merges_them(self):
        mod = self.add('Stamina', {'main.ips': b'PATCHEOF'}, part='exefs')['mods'][0]
        self.assertEqual((mod['valid'], mod['code'], mod['bad']), (False, False, False))
        self.assertIn('Code patches (an .ips)', mod['details'][1])
        state = self.ok('code', mod=mod['id'], on=True)
        self.assertTrue(state['mods'][0]['code'])
        self.assertIn('Stamina', state['code_line'])
        self.assertIn('next full build', state['code_line'])   # build_job composes them (code_mods.compose)
        self.assertFalse(self.call('code', mod=mod['id'], on='yes')['ok'])

    def test_the_player_s_own_backups_are_listed_and_removed_only_when_asked(self):
        backup = self.load.with_name('v143.backup-20260101-000000')
        (backup / 'Pack').mkdir(parents=True)
        (backup / 'Pack/A.pack.zs').write_bytes(b'mine')
        state = self.ok('state')
        self.assertEqual(state['backups']['names'], [backup.name])
        self.assertIn('Delete the 1 backups', state['backups']['confirm'])
        self.assertEqual(self.ok('remove_backups', names=['other'])['backups']['names'], [backup.name])
        self.assertTrue(backup.is_dir())
        self.assertIsNone(self.ok('remove_backups', names=[backup.name])['backups'])
        self.assertFalse(backup.exists())

    def test_another_version_s_app_is_said_in_words_and_takes_no_change(self):
        other = fake_app(self.tmp / 'old', version='v121')
        state = self.call('state', app=other)['result']
        self.assertEqual(state['reason'], 'Mods here are for Tears of the Kingdom 1.4.3 only: build that version to '
                                          'use them.')
        refused = self.call('add', app=other, path=str(self.source('A', {'Pack/A.pack.zs': b'x'})))
        self.assertFalse(refused['ok'])
        self.assertIn('1.4.3 only', refused['error'])

    def test_a_second_writer_is_refused_in_words(self):
        lock = self.home / 'Library/Application Support/MacNativeGames/Builder' / tab_mods.LOCK
        lock.parent.mkdir(parents=True)
        fd = os.open(lock, os.O_RDWR | os.O_CREAT)
        self.addCleanup(os.close, fd)
        fcntl.flock(fd, fcntl.LOCK_EX)
        answer = self.call('state')
        self.assertEqual(answer, {'ok': False, 'error': 'Another mods change is still running: try again when it ends.'})

    def test_after_a_build(self):
        self.assertIn('take their code out', self.ok('after_build', ok=False, error='… for another build of main …')['note'])
        self.assertEqual(self.ok('after_build', ok=False, error='disk full')['note'], '')
        self.assertEqual(self.ok('after_build', ok=True)['note'], '')         # nothing applied: nothing to say

    def test_chinese(self):
        with patch.dict(os.environ, MNG_LANG='zh'):
            state = self.ok('state')
        self.assertEqual(state['labels']['apply'], '应用模组')
        self.assertIn('模组都由你自己添加', state['note'])

    def test_every_refusal_and_reason_of_the_models_has_words(self):
        sources = (REPO / 'tools/builder/mod_find.py').read_text() + (REPO / 'tools/builder/mod_archive.py').read_text()
        reasons = set(re.findall(r"Refused\('([a-z_]+)'", sources))
        self.assertTrue(reasons)
        self.assertEqual(sorted(r for r in reasons if f'refused.{r}' not in tab_mods.TEXT), [])
        for name in ('misspelt', 'other_version', 'only_adds', 'all_taken', 'no_romfs', 'no_app_romfs', 'unchecked'):
            self.assertIn(f'invalid.{name}', tab_mods.TEXT)
        for name in ('ok', 'other_build', 'not_for_game', 'unchecked', 'whole_module'):
            self.assertIn(f'code.{name}', tab_mods.TEXT)
        for name in ('ok', 'nothing', 'set_aside', 'running', 'no_space', 'install_failed'):
            self.assertIn(f'applied.{name}', tab_mods.TEXT)

    def test_the_command_line_with_mng_home(self):
        env = dict(os.environ, MNG_HOME=str(self.home), PYTHONPATH=str(REPO))
        done = subprocess.run([sys.executable, '-m', 'tools.builder_jobs', 'tab', 'mods', 'add', '--app', str(self.app),
                               '--args', json.dumps({'path': str(self.source('A', {'UI/C.bfres': b'd'}))})],
                              cwd=REPO, env=env, capture_output=True, text=True)
        answer = json.loads(done.stdout)
        self.assertTrue(answer['ok'], answer)
        self.assertEqual(answer['result']['status']['text'], 'Added.')
        self.assertTrue((self.home / 'Library/Application Support/MacNativeGames/TOTK/mod-library/v143').is_dir())
        self.assertFalse(self.elsewhere.exists())


if __name__ == '__main__':
    unittest.main()
