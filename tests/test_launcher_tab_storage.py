"""MNG Launcher's Storage tab (tools/builder_jobs/tab_storage.py): the work folder the app records and the game's shader
cache, their sizes, cleaning (only their contents, never a game, save, app or this checkout) and moving the work folder,
with the refusals in words. Everything happens under a temporary folder and a temporary home."""
import fcntl
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
from tools.builder_jobs import tab_storage, tabs  # noqa: E402
from test_launcher_tabs import fake_app  # noqa: E402


def write(path, data=b'x'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def record(app, work):
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    info.update(MNGWorkFolder=str(work), MNGBuildPython='/usr/bin/python3', MNGSourceRepo=str(REPO))
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))


class StorageTab(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.tmp)
        for patch in (mock.patch.object(storage, 'SYSTEM', frozenset()),       # the temporary folder is under /private
                      mock.patch.object(tab_storage, 'running_builds', lambda: []),
                      mock.patch.dict(os.environ, {'MNG_LANG': 'en'})):
            patch.start()
            self.addCleanup(patch.stop)
        self.home = self.tmp / 'home'
        self.apps = self.tmp / 'Applications'
        self.app = fake_app(self.apps)
        self.work = self.tmp / 'MacNativeGames-work/totk'
        record(self.app, self.work)
        write(self.work / 'romfs/a.bin', b'1234')
        write(self.work / 'logs/b.log', b'12')
        self.shader = self.home / 'Library/Application Support/MacNativeGames/TOTK/shader-cache'
        write(self.shader / 'rt-1/lib.metallib', b'123')
        self.profile = self.home / 'Library/Application Support/MacNativeGames/TOTK/profile'
        write(self.profile / 'save/0/progress.sav', b'save')

    def call(self, action, **args):
        return tabs.call('storage', action, self.app, args, home=self.home)

    def ok(self, action, **args):
        answer = self.call(action, **args)
        self.assertTrue(answer['ok'], answer)
        return answer['result']

    def refused(self, action, **args):
        answer = self.call(action, **args)
        self.assertFalse(answer['ok'], answer)
        return answer['error']

    # --- where the folders are ---
    def test_the_work_folder_is_the_one_the_app_records_and_the_shader_cache_the_launcher_s(self):
        folders = {f['id']: f for f in self.ok('state')['folders']}
        self.assertEqual(folders['work']['path'], str(self.work))
        self.assertEqual(folders['work']['kind'], 'recorded')
        self.assertIn('MNGWorkFolder', folders['work']['where'])
        self.assertIn('game files', folders['work']['note'])            # cleaning it can mean a full build again
        self.assertEqual(folders['shader']['path'], str(self.shader))
        self.assertIn('TOTK_SHADER_CACHE', folders['shader']['where'])
        self.assertEqual((folders['work']['movable'], folders['shader']['movable']), (True, False))
        self.assertIsNone(folders['work']['size'])                      # state never walks a folder

    def test_a_deleted_recorded_folder_and_an_app_recording_none_are_said(self):
        shutil.rmtree(self.work)
        self.assertEqual(self.ok('state')['folders'][0]['kind'], 'recreated')
        record(self.app, self.tmp / 'gone/disk/totk')
        self.assertEqual(self.ok('state')['folders'][0]['kind'], 'unreachable')
        self.assertIn('cannot be found', self.refused('clean', folder='work'))
        info = plistlib.loads((self.app / 'Contents/Info.plist').read_bytes())
        del info['MNGWorkFolder']
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        work = self.ok('state')['folders'][0]
        self.assertEqual((work['kind'], work['path']), ('unrecorded', str(self.apps / 'TOTK_build')))

    def test_state_asks_the_work_folder_question_without_measuring(self):
        # the window measures only when asked (user 2026-11-12), so Clean must have its question before any size
        work, shader = self.ok('state')['folders']
        self.assertIsNone(work['size'])
        self.assertTrue(work['confirm'].endswith('Clean it?'))
        self.assertEqual(shader['confirm'], '')

    def test_a_measured_size_shows_again_until_the_folder_is_cleaned(self):
        # once measured, the page shows that size when it opens, with when it was measured (user 2026-11-12)
        self.assertIsNone(self.ok('state')['folders'][0]['size'])
        self.ok('measure', folder='work')
        work = self.ok('state')['folders'][0]
        self.assertEqual((work['size']['bytes'], work['size']['files']), (6, 2))
        self.assertIn('6 B in 2 files', work['detail'])
        self.assertIn('measured', work['detail'])
        self.assertTrue((self.home / 'Library/Application Support/MacNativeGames/Builder/storage-sizes.json').is_file())
        self.ok('clean', folder='work')
        self.assertIsNone(self.ok('state')['folders'][0]['size'])         # the old size is gone with the files

    # --- measure ---
    def test_measure_counts_one_folder_and_writes_the_question_to_ask(self):
        work = self.ok('measure', folder='work')
        self.assertEqual((work['size']['bytes'], work['size']['files']), (6, 2))
        self.assertEqual(work['detail'], '6 B in 2 files')
        # the work folder's question says what cleaning costs the next update and asks (user 2026-11-12)
        self.assertIn('the next update first makes these files again', work['confirm'])
        self.assertIn('game files and keys (a full build)', work['confirm'])
        self.assertTrue(work['confirm'].endswith('Clean it?'))
        shader = self.ok('measure', folder='shader')
        self.assertIn('1 files', shader['confirm'])                     # the shader cache keeps the plain question
        self.assertEqual(self.ok('measure', folder='shader')['size']['files'], 1)
        self.assertIn('work', self.refused('measure', folder='saves'))

    def test_a_missing_folder_does_not_exist_yet(self):
        shutil.rmtree(self.shader)
        self.assertEqual(self.ok('measure', folder='shader')['detail'], 'Does not exist yet.')

    # --- clean ---
    def test_clean_empties_the_folder_and_keeps_it_and_the_saves(self):
        done = self.ok('clean', folder='work')
        self.assertEqual((done['bytes'], done['files']), (6, 2))
        self.assertEqual(done['message'], 'Removed 6 B in 2 files.')
        self.assertTrue(self.work.is_dir())
        self.assertEqual(list(self.work.iterdir()), [])
        self.ok('clean', folder='shader')
        self.assertEqual(list(self.shader.iterdir()), [])
        self.assertTrue((self.profile / 'save/0/progress.sav').is_file())
        self.assertTrue((self.app / 'Contents/Info.plist').is_file())

    def test_a_work_folder_recorded_on_the_home_the_saves_or_the_apps_is_refused_in_words(self):
        for where, words in ((self.home, 'home folder'), (self.profile.parent, 'game, its app or its saves'),
                             (self.apps, 'game, its app or its saves')):
            record(self.app, where)
            self.assertIn(words, self.refused('clean', folder='work'))
        self.assertTrue((self.profile / 'save/0/progress.sav').is_file())
        # this checkout is protected too (checked on the guard itself: a clean is never tried on it)
        ctx = tabs.context(self.app, self.home)
        self.assertEqual(storage.refuse_reason(REPO, self.home, tab_storage.protected(ctx)), 'protected')

    def test_a_running_game_or_build_stops_a_clean(self):
        slot = self.home / 'Library/Application Support/MacNativeGames/TOTK/game-slot'
        slot.mkdir(parents=True)
        fd = os.open(slot / 'lock', os.O_RDWR | os.O_CREAT)
        self.addCleanup(os.close, fd)
        fcntl.flock(fd, fcntl.LOCK_EX)          # flock locks belong to the open file: another open() is refused
        self.assertIn('game is running', self.refused('clean', folder='shader'))
        fcntl.flock(fd, fcntl.LOCK_UN)
        with mock.patch.object(tab_storage, 'running_builds', lambda: ['python build_app.py x']):
            self.assertIn('build or an update is running', self.refused('clean', folder='work'))
            self.ok('clean', folder='shader')
        self.assertTrue((self.work / 'romfs/a.bin').is_file())

    def test_the_build_check_finds_build_and_update_scripts_only(self):
        self.assertTrue(tab_storage.BUILDS.search('/usr/bin/python3 /x/build_app.py a.xci --work w'))
        self.assertTrue(tab_storage.BUILDS.search('python3.14 update_runtime.py "/A/T.app"'))
        self.assertFalse(tab_storage.BUILDS.search('vim tools/builder_jobs/build_job.py'))

    # --- move ---
    def test_move_asks_first_then_moves_and_names_the_command_that_records_it(self):
        target = self.tmp / 'Other/totk'
        asked = self.ok('move', target=str(target))
        self.assertIn('Move everything in', asked['confirm'])
        self.assertFalse(target.exists())                              # nothing happens before the confirmation
        moved = self.ok('move', target=str(target), confirmed=True)
        self.assertEqual(moved['moved'], 2)
        self.assertTrue((target / 'romfs/a.bin').is_file())
        self.assertEqual(list(self.work.iterdir()), [])
        self.assertIn('update_runtime.py', moved['command'])
        self.assertIn(f'--work {target}', moved['command'])
        self.assertIn('still records', moved['message'])

    def test_move_refuses_the_places_a_work_folder_may_not_be(self):
        full = self.tmp / 'Full'
        write(full / 'x')
        cases = ((self.apps / 'work', 'folder that holds your apps'), (REPO / 'work', 'MacNativeGames checkout'),
                 (self.work / 'inner', 'inside the old one'), (full, 'not empty'), (self.home, 'home folder'),
                 (self.profile / 'w', 'game, its app or its saves'), (full / 'x', 'a file'), ('relative/w', 'does not exist'))
        for target, words in cases:
            self.assertIn(words, self.refused('move', target=str(target)), target)   # refused before the question
        self.assertTrue((self.work / 'romfs/a.bin').is_file())
        self.assertFalse((REPO / 'work').exists())

    def test_an_empty_work_folder_has_nothing_to_move(self):
        shutil.rmtree(self.work)
        answer = self.ok('move', target=str(self.tmp / 'Other'), confirmed=True)
        self.assertEqual((answer['moved'], answer['confirm']), (0, ''))
        self.assertIn('nothing', answer['message'])

    def test_the_page_speaks_chinese(self):
        with mock.patch.dict(os.environ, {'MNG_LANG': 'zh'}):
            self.assertEqual(self.ok('state')['title'], '缓存')
            self.assertIn('游戏文件', self.ok('state')['folders'][0]['note'])
            record(self.app, self.home)
            self.assertIn('用户文件夹', self.refused('clean', folder='work'))


if __name__ == '__main__':
    unittest.main()
