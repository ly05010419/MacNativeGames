"""tools/builder/gui_saves.py (S2.4): the window's Saves tab. The save is listed, a backup writes plain files and
backup.json, and a restore backs the current save up first, shows what it will overwrite and never deletes a backup."""
import json
import tempfile
import time
import unittest
from datetime import datetime
from pathlib import Path

from builder_tk_base import Base

from tools.builder import saves
from tools.builder.strings import EN, text

NOW = datetime(2026, 8, 17, 10, 30, 0).timestamp()


def wait(base, predicate, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        base.root.update()
        if predicate():
            return True
        time.sleep(0.02)
    return False


def buttons(widget):
    """Every FlatButton under a widget, in order."""
    found = []
    for child in widget.winfo_children():
        button = getattr(child, 'button', None)
        if button is not None:
            found.append(button)
        found.extend(buttons(child))
    return found


class SavesTests(Base):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name)
        self.root_dir = self.home / 'Documents' / 'MacNativeGames-Backups'
        self.profile = self.home / 'Library/Application Support/MacNativeGames/TOTK/profile'
        self.profile.mkdir(parents=True)
        (self.profile / 'progress.sav').write_bytes(b'old')
        (self.profile / 'settings.bin').write_bytes(b'cfg')
        self.start(home=self.home)
        self.ctl.saved = {'totk': {'game': str(self.home / 'Tears.xci')},
                          'botw': {'game': str(self.home / 'BotW.xci')}}
        self.loaded('totk')

    def open_tab(self):
        self.view.tabs.select('saves')
        self.assertTrue(wait(self, lambda: self.view.saves.rows.winfo_children()))

    def test_the_tab_lists_the_save_and_what_it_holds(self):
        self.open_tab()
        lines = self.labels(self.view.saves.rows)
        self.assertIn(text('gui.saves.profile'), lines)
        self.assertTrue(any('in 2 files' in line for line in lines), lines)
        self.assertIn(str(self.profile), lines)

    def test_a_game_without_a_save_says_there_is_none_yet(self):
        self.open_tab()
        self.radio('botw').invoke()
        self.view.tabs.select('saves')
        self.assertTrue(wait(self, lambda: any('no save yet' in line for line in self.labels(self.view.saves.rows))))
        self.assertTrue(any('no save yet' in line for line in self.labels(self.view.saves.rows)))

    def test_a_backup_writes_the_files_and_a_manifest(self):
        self.open_tab()
        back_up = [b for b in buttons(self.view.saves.rows) if 'Back up' in b.label.cget('text')]
        self.assertTrue(back_up, 'the save row must offer a backup')
        self.answer = str(self.root_dir)
        back_up[0].invoke()
        self.assertTrue(wait(self, lambda: EN['gui.saves.backed_up'].split('{')[0].strip()
                             in self.view.saves.status.cget('text')),
                        self.view.saves.status.cget('text'))
        made = next(self.root_dir.iterdir())
        self.assertTrue((made / 'progress.sav').is_file())
        manifest = json.loads((made / 'backup.json').read_text())
        self.assertEqual(manifest['game'], 'totk')
        self.assertEqual(manifest['source'], str(self.profile))
        self.assertEqual({item['path'] for item in manifest['files']}, {'progress.sav', 'settings.bin'})
        self.assertIn(EN['gui.saves.backed_up'].split('{')[0].strip(),
                      self.view.saves.status.cget('text'))

    def test_restore_backs_the_current_save_up_first_and_then_writes_the_backup(self):
        made, error = saves.backup(self.profile, self.root_dir, 'totk', 'totk', NOW)
        self.assertIsNone(error)
        (self.profile / 'progress.sav').write_bytes(b'changed')
        (self.profile / 'settings.bin').unlink()
        self.open_tab()
        self.answer = str(made.directory)
        self.confirm_answer = True
        self.view.saves.restore_button.invoke()
        self.assertTrue(wait(self, lambda: 'Restored' in self.view.saves.status.cget('text')),
                        self.view.saves.status.cget('text'))
        self.assertEqual((self.profile / 'progress.sav').read_bytes(), b'old')
        self.assertEqual((self.profile / 'settings.bin').read_bytes(), b'cfg')
        self.assertTrue(self.asked, 'a restore must show what it overwrites and ask')
        self.assertIn('files will be written', self.asked[0])
        self.assertTrue(any('pre-restore' in path.name for path in self.root_dir.iterdir()))
        self.assertTrue(made.directory.is_dir())

    def test_a_backup_of_another_game_is_refused(self):
        made, _ = saves.backup(self.profile, self.root_dir, 'botw', 'botw', NOW)
        (self.profile / 'progress.sav').write_bytes(b'changed')
        self.open_tab()
        self.answer = str(made.directory)
        self.confirm_answer = True
        self.view.saves.restore_button.invoke()
        self.assertTrue(wait(self, lambda: 'that game' in self.view.saves.status.cget('text')
                             or 'That backup is for' in self.view.saves.status.cget('text')),
                        self.view.saves.status.cget('text'))
        self.assertEqual((self.profile / 'progress.sav').read_bytes(), b'changed')

    def test_a_folder_that_is_not_a_backup_is_refused(self):
        plain = self.home / 'plain'
        plain.mkdir()
        self.open_tab()
        self.answer = str(plain)
        self.view.saves.restore_button.invoke()
        self.assertTrue(wait(self, lambda: 'not a backup' in self.view.saves.status.cget('text')),
                        self.view.saves.status.cget('text'))
        self.assertFalse(self.asked)


if __name__ == '__main__':
    unittest.main()
