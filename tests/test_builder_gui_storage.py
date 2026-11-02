"""tools/builder/gui_storage.py (S2.2): the window's Storage tab. A real (withdrawn) Tk window and the scripted
controller: the two caches are shown with what they hold, cleaning asks first and empties a folder, a chosen folder is
saved to settings.json and what was there is moved."""
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from types import SimpleNamespace

from builder_tk_base import Base

from tools.builder import storage
from tools.builder.strings import text


def wait(base, predicate, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        base.root.update()
        if predicate():
            return True
        time.sleep(0.02)
    return False


class StorageTests(Base):
    def setUp(self):
        # The temporary home is under /var on macOS; the system-name guard is storage.py's own test.
        self.patch = mock.patch.object(storage, 'SYSTEM', frozenset())
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name)
        self.game = self.home / 'Tears.xci'
        self.game.write_bytes(b'game')
        self.start(home=self.home)
        self.ctl.saved = {'totk': {'game': str(self.game), 'keys': '/k'}}
        self.loaded('totk')

    def open_tab(self):
        self.view.tabs.select('storage')

    def row(self, key):
        return self.view.storage.rows[key]

    def test_the_tab_shows_the_build_cache_beside_the_game_and_the_shader_cache_in_application_support(self):
        self.open_tab()
        self.assertEqual(self.row('work')['path'].cget('text'), str(self.home / 'TOTK_build'))
        self.assertEqual(self.row('shader')['path'].cget('text'),
                         str(self.home / 'Library/Application Support/MacNativeGames/TOTK/shader-cache'))
        self.assertTrue(wait(self, lambda: text('gui.storage.missing') in self.row('work')['detail'].cget('text')))

    def test_a_cache_that_exists_is_measured(self):
        cache = self.home / 'TOTK_build/rt-x'
        cache.mkdir(parents=True)
        (cache / 'pipeline.bin').write_bytes(b'12345')
        self.open_tab()
        self.assertTrue(wait(self, lambda: '5 B' in self.row('work')['detail'].cget('text')))

    def test_cleaning_asks_first_then_empties_the_folder(self):
        cache = self.home / 'TOTK_build/rt-x'
        cache.mkdir(parents=True)
        (cache / 'pipeline.bin').write_bytes(b'12345')
        self.open_tab()
        self.assertTrue(wait(self, lambda: '5 B' in self.row('work')['detail'].cget('text')))
        self.confirm_answer = True
        self.row('work')['clean'].invoke()
        self.assertTrue(self.asked, 'cleaning must ask before deleting anything')
        self.assertIn(str(self.home / 'TOTK_build'), self.asked[0])
        self.assertTrue(wait(self, lambda: 'Removed' in self.view.storage.status.cget('text')),
                        self.view.storage.status.cget('text'))
        self.assertTrue((self.home / 'TOTK_build').is_dir())
        self.assertEqual(list((self.home / 'TOTK_build').iterdir()), [])

    def test_a_refused_folder_is_reported_and_not_cleaned(self):
        keep = self.home / 'keep.bin'
        keep.write_bytes(b'keep')
        self.ctl.saved_paths_value = SimpleNamespace(work=str(self.home))
        self.open_tab()
        self.confirm_answer = True
        self.row('work')['clean'].invoke()
        self.assertTrue(wait(self, lambda: 'Could not clean' in self.view.storage.status.cget('text')),
                        self.view.storage.status.cget('text'))
        self.assertTrue(keep.is_file())

    def test_choosing_a_folder_saves_it_and_moves_the_old_cache(self):
        cache = self.home / 'TOTK_build'
        cache.mkdir()
        (cache / 'pipeline.bin').write_bytes(b'12345')
        self.open_tab()
        self.assertTrue(wait(self, lambda: '5 B' in self.row('work')['detail'].cget('text')))
        target = self.home / 'new-build'
        self.answer = str(target)
        self.confirm_answer = True
        self.row('work')['choose'].invoke()
        self.assertIn(('save_paths', {'work': str(target)}), self.ctl.calls)
        self.assertTrue(wait(self, lambda: 'Moved' in self.view.storage.status.cget('text')),
                        self.view.storage.status.cget('text'))
        self.assertTrue((target / 'pipeline.bin').is_file())

    def test_a_chosen_folder_that_is_the_home_folder_is_refused(self):
        self.open_tab()
        self.answer = str(self.home)
        self.row('work')['choose'].invoke()
        self.assertNotIn('save_paths', [call[0] for call in self.ctl.calls if isinstance(call, tuple)])
        self.assertIn(text('gui.storage.refused.home'), self.view.storage.status.cget('text'))

    def test_the_shader_cache_is_where_the_launcher_writes_it(self):
        chosen = self.home / 'elsewhere'
        chosen.mkdir()
        self.ctl.saved_paths_value = SimpleNamespace(work=None, extra={'shader_cache': str(chosen)})
        self.open_tab()
        self.assertNotEqual(self.row('shader')['path'].cget('text'), str(chosen))   # the launcher reads no such setting
        self.assertIsNone(self.row('shader')['choose'])


if __name__ == '__main__':
    unittest.main()
