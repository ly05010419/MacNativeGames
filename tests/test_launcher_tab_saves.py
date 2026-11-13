"""MNG Launcher's Saves page (tools/builder_jobs/tab_saves.py): the old window's sources, backups and restore, with the
same refusals, over a temporary home only (the player's saves are never read or written here)."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tests.test_launcher_tabs import fake_app  # noqa: E402
from tools.builder_jobs import tab_saves, tabs  # noqa: E402

NX_TOTK = '0100F2C0115B6000'


class SavesPage(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.home = self.tmp / 'home'
        self.app = fake_app(self.tmp)
        env = mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if k != 'TOTK_PROFILE_DIR'}, clear=True)
        env.start()
        self.addCleanup(env.stop)
        self.data = self.home / 'Library/Application Support/MacNativeGames/TOTK'
        self.profile = self.data / 'profile'
        self.write(self.profile / 'save/0/progress.sav', b'slot zero')
        self.write(self.profile / 'account.uid', b'uid')

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def call(self, action, **args):
        return tabs.call('saves', action, self.app, args, home=self.home)

    def ok(self, action, **args):
        answer = self.call(action, **args)
        self.assertTrue(answer['ok'], answer)
        return answer['result']

    def test_state_lists_the_profile_and_nxnative_titles_and_writes_nothing(self):
        self.write(self.home / 'Library/Application Support/nxnative' / NX_TOTK / 'save.bin', b'nx')
        (self.home / 'Library/Application Support/nxnative/not-a-title').mkdir()
        before = sorted(p for p in self.home.rglob('*'))
        result = self.ok('state')
        self.assertEqual(sorted(p for p in self.home.rglob('*')), before)
        self.assertEqual([(s['kind'], s['title_id']) for s in result['sources']], [('profile', '0100F2C0115B6000'), ('nxnative', NX_TOTK)])
        self.assertEqual(result['sources'][0]['files'], 2)
        self.assertTrue(result['sources'][0]['can_backup'])
        self.assertEqual(result['backups'], [])
        self.assertFalse(result['running'])

    def test_a_missing_profile_cannot_be_backed_up(self):
        shutil.rmtree(self.profile)
        result = self.ok('state')
        self.assertFalse(result['sources'][0]['can_backup'])
        self.assertFalse(self.call('backup')['ok'])

    def test_backup_then_list_with_details(self):
        made = self.ok('backup')['backup']
        self.assertTrue(made['name'].startswith('totk-'))
        self.assertTrue(Path(made['path']).is_relative_to(self.home / 'Documents/MacNativeGames-Backups'))
        self.assertEqual((made['files'], made['hashed']), (2, 2))
        listed = self.ok('state')['backups']
        self.assertEqual([b['path'] for b in listed], [made['path']])
        self.assertEqual(listed[0]['game'], 'totk')

    def test_backup_labels_and_folders(self):
        made = self.ok('backup', label='before-boss', root=str(self.tmp / 'elsewhere'))['backup']
        self.assertTrue(made['name'].startswith('before-boss-'))
        self.assertTrue(Path(made['path']).is_relative_to(self.tmp / 'elsewhere'))
        self.assertFalse(self.call('backup', label='../out')['ok'])
        inside = self.call('backup', root=str(self.profile / 'b'))
        self.assertFalse(inside['ok'])
        self.assertIn('inside the save folder', inside['error'])
        self.assertFalse(self.call('backup', kind='nxnative', title_id=NX_TOTK)['ok'])

    def test_plan_then_restore_writes_only_what_differs_and_backs_up_first(self):
        made = self.ok('backup')['backup']
        self.write(self.profile / 'save/0/progress.sav', b'slot zero, later')
        self.write(self.profile / 'save/1/new.sav', b'only now')
        plan = self.ok('plan', backup=made['path'])
        self.assertEqual(plan['write'], ['save/0/progress.sav'])
        self.assertEqual(plan['same'], ['account.uid'])
        self.assertEqual(plan['kept'], ['save/1/new.sav'])
        self.assertIn('1 files will be written', plan['message'])
        done = self.ok('restore', backup=made['path'], plan_id=plan['plan_id'])
        self.assertEqual(done['written'], 1)
        self.assertEqual((self.profile / 'save/0/progress.sav').read_bytes(), b'slot zero')
        self.assertEqual((self.profile / 'save/1/new.sav').read_bytes(), b'only now')      # never deleted
        pre = Path(done['pre_backup'])
        self.assertIn('totk-pre-restore-', pre.name)
        self.assertEqual((pre / 'save/0/progress.sav').read_bytes(), b'slot zero, later')

    def test_restore_refuses_a_changed_plan(self):
        made = self.ok('backup')['backup']
        plan = self.ok('plan', backup=made['path'])
        self.write(self.profile / 'save/0/progress.sav', b'the game saved meanwhile')
        refused = self.call('restore', backup=made['path'], plan_id=plan['plan_id'])
        self.assertFalse(refused['ok'])
        self.assertEqual((self.profile / 'save/0/progress.sav').read_bytes(), b'the game saved meanwhile')
        self.assertFalse(self.call('restore', backup=made['path'])['ok'])

    def test_restore_refuses_while_the_game_slot_is_held(self):
        made = self.ok('backup')['backup']
        self.write(self.profile / 'save/0/progress.sav', b'changed')
        plan = self.ok('plan', backup=made['path'])
        lock = self.data / 'game-slot/lock'
        self.write(lock, b'')
        holder = subprocess.Popen([sys.executable, '-c', 'import fcntl,sys,time; f=open(sys.argv[1]); '
                                   'fcntl.flock(f, fcntl.LOCK_EX); print("held", flush=True); time.sleep(30)', str(lock)],
                                  stdout=subprocess.PIPE, text=True)
        self.addCleanup(holder.stdout.close)
        self.addCleanup(holder.wait)
        self.addCleanup(holder.kill)
        self.assertEqual(holder.stdout.readline().strip(), 'held')
        self.assertTrue(self.ok('state')['running'])
        refused = self.call('restore', backup=made['path'], plan_id=plan['plan_id'])
        self.assertFalse(refused['ok'])
        self.assertIn('running', refused['error'])
        self.assertEqual((self.profile / 'save/0/progress.sav').read_bytes(), b'changed')
        self.assertEqual(len(self.ok('state')['backups']), 1)                               # no pre-restore copy either

    def test_restore_refuses_other_games_and_non_backups(self):
        self.assertIn('not a backup', self.call('plan', backup=str(self.profile))['error'])
        made = Path(self.ok('backup')['backup']['path'])
        manifest = json.loads((made / 'backup.json').read_text())
        (made / 'backup.json').write_text(json.dumps(dict(manifest, game='botw')))
        self.assertIn('Breath of the Wild', self.call('plan', backup=str(made))['error'])

    def test_restore_refuses_a_tampered_backup_before_writing(self):
        made = Path(self.ok('backup')['backup']['path'])
        self.write(self.profile / 'save/0/progress.sav', b'current')
        plan = self.ok('plan', backup=str(made))
        (made / 'save/0/progress.sav').write_bytes(b'tampered!')                          # same size, other bytes
        refused = self.call('restore', backup=str(made), plan_id=plan['plan_id'])
        self.assertFalse(refused['ok'])
        self.assertEqual((self.profile / 'save/0/progress.sav').read_bytes(), b'current')

    def test_reveal_opens_the_backup_folder(self):
        with mock.patch.object(tab_saves.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '', '')) as run:
            result = self.ok('reveal')
        root = self.home / 'Documents/MacNativeGames-Backups'
        self.assertTrue(root.is_dir())
        run.assert_called_once()
        self.assertEqual(run.call_args.args[0], ['/usr/bin/open', str(root)])
        self.assertEqual(result['opened'], str(root))
        self.assertFalse(self.call('reveal', path=str(self.profile))['ok'])


if __name__ == '__main__':
    unittest.main()
