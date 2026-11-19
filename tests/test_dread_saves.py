"""Metroid Dread's Saves page, end to end over a temporary home (the player's saves are never read or written here).

The page was off for Dread until 2026-11-19 because a comment claimed its launcher read neither TOTK_PROFILE_DIR nor
TOTK_GAME_SLOT_DIR. It reads both (games/dread/app/launcher.m), so the page works as Ocarina of Time's does. The tests
here are what that claim could not survive: the backup-restore round trip on a Dread app, the refusal while another
process holds the game slot, and guards that fail if Dread is dropped from the tables again -- each table checked on
its own, since Python and Swift agreeing means nothing when both leave Dread out.
"""
import fcntl
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.builder import saves  # noqa: E402
from tools.builder.games import PAGES, PAGE_GAMES  # noqa: E402
from tools.builder_jobs import tab_saves, tabs  # noqa: E402

LAUNCHER = REPO / 'games/dread/app/launcher.m'
MODEL_SWIFT = REPO / 'tools/builder_app/Sources/MNGBuilder/Model.swift'
SAVE = 'save/0/progress.sav'


def dread_app(folder, version='v210'):
    """An app tools/builder reads as Metroid Dread (package_dread.py writes Contents/Resources/dread)."""
    app = Path(folder) / 'Metroid Dread.app'
    (app / 'Contents').mkdir(parents=True)
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'org.macnativegames.dread'}))
    (app / f'Contents/Resources/dread/extracted/{version}').mkdir(parents=True)
    return app


class DreadSaves(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.home = self.tmp / 'home'
        self.app = dread_app(self.tmp)
        # no TOTK_PROFILE_DIR in the environment: the page must find Dread's own folder by itself
        env = mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if k != 'TOTK_PROFILE_DIR'}, clear=True)
        env.start()
        self.addCleanup(env.stop)
        self.data = self.home / 'Library/Application Support/MacNativeGames/Dread'
        self.profile = self.data / 'profile'
        self.write(self.profile / SAVE, b'samus at the start')
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

    def test_the_page_reads_dreads_own_profile(self):
        result = self.ok('state')
        self.assertEqual(tabs.context(self.app, self.home).spec.id, 'dread')
        self.assertEqual([s['kind'] for s in result['sources']], ['profile'])
        self.assertEqual(result['sources'][0]['files'], 2)
        self.assertEqual(Path(result['sources'][0]['path']), self.profile)
        self.assertFalse(result['running'])

    def test_a_backup_is_restored_byte_for_byte(self):
        made = self.ok('backup')['backup']
        self.write(self.profile / SAVE, b'samus much later')
        plan = self.ok('plan', backup=made['path'])
        self.ok('restore', backup=made['path'], plan_id=plan['plan_id'])
        self.assertEqual((self.profile / SAVE).read_bytes(), b'samus at the start')
        # the save as it was before the restore is kept, so nothing is lost either way
        pre = sorted(p for p in Path(made['path']).parent.glob('*pre-restore*') if p.is_dir())
        self.assertTrue(pre, 'no pre-restore copy was made')
        self.assertEqual((pre[-1] / SAVE).read_bytes(), b'samus much later')

    def test_restore_is_refused_while_another_process_holds_the_slot_and_the_save_is_left_alone(self):
        made = self.ok('backup')['backup']
        self.write(self.profile / SAVE, b'samus much later')
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
        self.assertTrue(tab_saves.slot_held(self.data), 'the slot is held but the page does not see it')
        self.assertTrue(self.ok('state')['running'])
        refused = self.call('restore', backup=made['path'], plan_id=plan['plan_id'])
        self.assertFalse(refused['ok'], refused)
        self.assertEqual((self.profile / SAVE).read_bytes(), b'samus much later', 'the refused restore wrote anyway')


class DreadStaysWired(unittest.TestCase):
    """What the regression of 2026-11-16 needed: each side asserted on its own, and against the launcher.

    tab_saves.slot_held takes the lock only to see whether it is free and lets it go at once, and restore holds nothing
    while it writes: these prove the refusal when the slot is already held, not exclusion against a game that starts
    during a restore. That is the common code's limit, not Dread's.
    """
    def test_the_launcher_sets_the_variables_the_page_follows(self):
        text = LAUNCHER.read_text()
        self.assertRegex(text, r'setenv\("TOTK_PROFILE_DIR"', 'the Saves page reads the folder this sets')
        self.assertRegex(text, r'setenv\("TOTK_GAME_SLOT_DIR"', 'the refusal reads the slot this sets')
        self.assertEqual(saves.PROFILE_ENV['dread'], 'TOTK_PROFILE_DIR')

    def test_the_page_follows_that_variable_when_the_game_sets_it(self):
        """Dropping 'dread' from PROFILE_ENV makes profile_dir ignore the launcher's folder and fall back."""
        with tempfile.TemporaryDirectory() as tmp:
            elsewhere = Path(tmp) / 'somewhere/profile'
            got = saves.profile_dir(PAGE_GAMES['dread'], tmp, {'TOTK_PROFILE_DIR': str(elsewhere)})
            self.assertEqual(got, elsewhere)

    def test_the_default_folder_is_the_one_the_launcher_builds(self):
        """launcher.m: data = Application Support/MacNativeGames/Dread, profile = <data>/profile."""
        with tempfile.TemporaryDirectory() as tmp:
            got = saves.profile_dir(PAGE_GAMES['dread'], tmp, {})
            self.assertEqual(got, Path(tmp) / 'Library/Application Support/MacNativeGames/Dread/profile')

    def test_python_lists_saves_for_dread(self):
        self.assertIn('saves', PAGES['dread'])

    def test_swift_lists_saves_for_dread(self):
        row = re.search(r'"dread": \[(.*?)\]', MODEL_SWIFT.read_text())
        self.assertIsNotNone(row, 'Model.swift gamePages has no row for dread')
        self.assertIn('saves', [name.strip().strip('"') for name in row.group(1).split(',')])


if __name__ == '__main__':
    unittest.main()
