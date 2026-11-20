"""游戏卸载 on MNG Launcher's Storage page (tools/builder_jobs/uninstall.py, tab_storage.uninstall_plan / uninstall; user
2026-11-20): the app, its work folder, its game data image and the shader cache go for good; the saves and settings stay
unless asked; the player's own game files and keys never go (a folder holding any stays whole), nor another game's data;
nothing goes without the confirmation, nor while the game or a build runs. Under a temporary folder and home."""
import os
from pathlib import Path
import plistlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / 'tests'))
from tools.builder import storage  # noqa: E402
from tools.builder_jobs import tab_storage, tabs, uninstall  # noqa: E402
from test_launcher_tabs import fake_app  # noqa: E402


def write(path, data=b'x'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class Uninstall(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.tmp)
        deleted = []
        self.deleted = deleted
        for patch in (mock.patch.object(storage, 'SYSTEM', frozenset()),       # the temporary folder is under /private
                      mock.patch.object(tab_storage, 'running_builds', lambda: []),
                      mock.patch.object(uninstall, 'detach_delete', lambda paths: [uninstall._rmtree(p) or deleted.append(p) for p in paths]),
                      mock.patch.dict(os.environ, {'MNG_LANG': 'en'})):
            patch.start()
            self.addCleanup(patch.stop)
        self.home = self.tmp / 'home'
        self.app = fake_app(self.tmp / 'Applications')
        self.work = self.tmp / 'MacNativeGames-work/totk'
        self.support = self.home / 'Library/Application Support/MacNativeGames'
        self.data = self.support / 'TOTK'
        self.gamedata = self.data / 'GameData'
        info = plistlib.loads((self.app / 'Contents/Info.plist').read_bytes())
        info.update(MNGWorkFolder=str(self.work), MNGGameDataFolder=str(self.gamedata))
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        write(self.work / 'romfs/a.bin')
        write(self.work / 'build/pipelines/0123abcd-8.keys')       # a pipeline archive's key list, not the player's keys
        write(self.gamedata / 'romfs.bin')
        write(self.data / 'shader-cache/rt-1/lib.metallib')
        write(self.data / 'profile/save/0/progress.sav', b'save')
        write(self.data / 'graphics-canvas.txt', b'3')
        write(self.support / 'BotW/profile/save/0/progress.sav', b'other game')

    def call(self, action, **args):
        return tabs.call('storage', action, self.app, args, home=self.home)

    def ok(self, action, **args):
        answer = self.call(action, **args)
        self.assertTrue(answer['ok'], answer)
        return answer['result']

    def test_the_question_lists_what_goes_and_removes_nothing(self):
        plan = self.ok('uninstall_plan')
        self.assertEqual([i['id'] for i in plan['items']], ['app', 'work', 'gamedata', 'shader'])
        self.assertTrue(all(not i['stays'] for i in plan['items']))
        self.assertIn('not to the Trash', plan['confirm'])
        self.assertIn('Saves and settings stay', plan['confirm'])
        self.assertIn('never deleted', plan['confirm'])
        self.assertEqual(self.ok('uninstall')['items'], plan['items'])      # unconfirmed: the same question
        self.assertTrue(self.app.is_dir() and self.work.is_dir() and self.gamedata.is_dir())

    def test_it_removes_the_app_and_its_folders_and_keeps_the_saves(self):
        result = self.ok('uninstall', confirmed=True)
        self.assertTrue(result['app_gone'])
        for gone in (self.app, self.work, self.gamedata, self.data / 'shader-cache'):
            self.assertFalse(gone.exists(), gone)
        self.assertEqual((self.data / 'profile/save/0/progress.sav').read_bytes(), b'save')
        self.assertTrue((self.data / 'graphics-canvas.txt').is_file())
        self.assertTrue((self.support / 'BotW/profile/save/0/progress.sav').is_file())
        self.assertEqual(len(self.deleted), 4)
        self.assertFalse(list(self.tmp.rglob(uninstall.TRASH + '*')))     # nothing renamed aside is left

    def test_saves_too_takes_the_whole_data_folder_and_no_other_game_s(self):
        plan = self.ok('uninstall_plan', saves=True)
        self.assertEqual([i['id'] for i in plan['items']], ['app', 'work', 'data'])   # its shader cache and image are inside
        self.ok('uninstall', saves=True, confirmed=True)
        self.assertFalse(self.data.exists())
        self.assertTrue((self.support / 'BotW/profile/save/0/progress.sav').is_file())
        self.assertTrue((self.support).is_dir())

    def test_a_folder_holding_the_player_s_game_files_stays_whole(self):
        for name in ('Tears of the Kingdom [0100F2C0115B6000].nsp', 'prod.keys'):
            write(self.work / 'games' / name, b'rom')
            result = self.ok('uninstall_plan')          # the question does not walk folders
            self.assertFalse(any(i['stays'] for i in result['items']))
        result = self.ok('uninstall', confirmed=True)
        self.assertTrue((self.work / 'games/prod.keys').is_file() and (self.work / 'romfs/a.bin').is_file())
        self.assertTrue(any('your game files' in k for k in result['kept']), result)
        self.assertFalse(self.app.exists())             # the rest goes
        self.assertFalse(self.gamedata.exists())

    def test_never_while_the_game_or_a_build_runs(self):
        with mock.patch.object(tab_storage, 'slot_taken', lambda ctx: True):
            answer = self.call('uninstall', confirmed=True)
        self.assertFalse(answer['ok'])
        self.assertIn('close it first', answer['error'])
        with mock.patch.object(tab_storage, 'running_builds', lambda: ['update_runtime.py']):
            self.assertFalse(self.call('uninstall', confirmed=True)['ok'])
        self.assertTrue(self.app.is_dir() and self.work.is_dir())

    def test_another_game_s_data_and_the_launcher_s_folder_are_never_taken(self):
        info = plistlib.loads((self.app / 'Contents/Info.plist').read_bytes())
        info['MNGGameDataFolder'] = str(self.support / 'BotW')
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        plan = {i['id']: i for i in self.ok('uninstall_plan')['items']}
        self.assertTrue(plan['gamedata']['stays'])
        self.ok('uninstall', confirmed=True)
        self.assertTrue((self.support / 'BotW/profile/save/0/progress.sav').is_file())

    def test_a_linked_data_folder_goes_with_what_it_points_to(self):
        real = self.tmp / 'elsewhere/TOTK'
        shutil.move(str(self.data), str(real))
        self.data.symlink_to(real)
        self.ok('uninstall', saves=True, confirmed=True)
        self.assertFalse(self.data.exists() or self.data.is_symlink())
        self.assertFalse(real.exists())
        self.assertTrue((self.tmp / 'elsewhere').is_dir())

    def test_the_detached_delete_touches_only_what_was_set_aside(self):
        keep = self.tmp / 'keep'; write(keep / 'f')
        uninstall._rmtree(keep)
        self.assertTrue((keep / 'f').is_file())


if __name__ == '__main__':
    unittest.main()
