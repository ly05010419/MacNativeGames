"""E9 S9.2: the Mods tab for Breath of the Wild 1.9.0. Its library and its load folder are BotW's; Apply, switching off
and back on never touch Tears of the Kingdom's files; BCML and TKMM packages are refused in words; code patches are not
used for BotW (the Mods page shows no code switch and says why). A fake built app (no game file), HOME inside the test
folder as in test_builder_mod_library."""
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from test_mod_manifest import fake_romfs
from tools.builder import mod_add, mod_code, mod_find, mod_library, mod_tools
from tools.builder.controller_mods import ModsState
from tools.builder.games import BOTW
from tools.builder.mod_library import Mod
from tools.builder.mod_tools import Validation
from tools.builder.ports import CommandRunner, RealClock
from tools.builder_jobs import tab_mods

REPO = Path(__file__).resolve().parents[1]
FILES = {'Actor/Pack/Weapon_Sword_001.sbactorpack': b'original sword', 'Pack/TitleBG.pack': b'original title' * 50}


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp).resolve()
        home = patch.dict(os.environ, HOME=str(self.tmp / 'home'))
        home.start()
        self.addCleanup(home.stop)
        _, folder_tool = mod_tools.tools(REPO)
        self.data = self.tmp / 'home/Library/Application Support/MacNativeGames'
        self.load = Path(folder_tool.mods_folder('botw190'))
        self.assertEqual(self.load, self.data / 'BotW/mods/botw190')
        self.app = self.tmp / 'Breath of the Wild.app'
        fake_romfs(self.app / 'Contents/Resources/totk/extracted/botw190/romfs', FILES)
        self.runner = CommandRunner(RealClock())
        self.library = mod_library.open_library(self.data / 'BotW', 'botw190', self.load)

    def source(self, name, files, exefs=None):
        root = self.tmp / 'mods' / name
        for path, data in files.items():
            (root / 'romfs' / path).parent.mkdir(parents=True, exist_ok=True)
            (root / 'romfs' / path).write_bytes(data)
        if exefs:
            (root / 'exefs').mkdir(parents=True)
            (root / 'exefs/main-1.9.0.pchtxt').write_text(exefs)
        return root

    def add(self, source):
        return mod_add.add(self.runner, REPO, self.app, 'botw190', self.library, source)

    def apply(self):
        return mod_library.apply(REPO, BOTW, self.app, 'botw190', self.library, self.data)


def snapshot(folder):
    return {p.relative_to(folder).as_posix(): p.read_bytes() for p in sorted(folder.rglob('*')) if p.is_file()}


class Library(Base):
    def test_supported_by_behaviour_now(self):
        self.assertTrue(mod_tools.supported(REPO, BOTW, 'botw190'))

    def test_apply_switch_off_and_on_never_touch_totk(self):
        totk = self.data / 'TOTK'
        (totk / 'mods/v143').mkdir(parents=True)
        (totk / 'mods/v143/manifest.tsv').write_text('theirs\n')
        (totk / 'mod-library/v143').mkdir(parents=True)
        (totk / 'mod-library/v143/library.json').write_text('{}\n')
        before = snapshot(totk)
        [mod_id] = self.add(self.source('Sword', {'Actor/Pack/Weapon_Sword_001.sbactorpack': b'modded sword'}))
        mod_library.set_enabled(self.library, mod_id, True)
        self.assertTrue(self.apply().ok)
        self.assertEqual((self.load / 'Actor/Pack/Weapon_Sword_001.sbactorpack').read_bytes(), b'modded sword')
        self.assertTrue((self.load / 'manifest.tsv').is_file())
        self.assertEqual(snapshot(totk), before)
        mod_library.set_enabled(self.library, mod_id, False)
        self.assertEqual(self.apply().reason, 'nothing')
        self.assertFalse(self.load.exists())                             # set aside, in BotW's folder
        self.assertTrue(any(p.name.startswith('botw190.') for p in (self.data / 'BotW/mods').iterdir()))
        self.assertEqual(snapshot(totk), before)
        mod_library.set_enabled(self.library, mod_id, True)
        self.assertTrue(self.apply().ok)
        self.assertEqual((self.load / 'Actor/Pack/Weapon_Sword_001.sbactorpack').read_bytes(), b'modded sword')
        self.assertEqual(snapshot(totk), before)

    def test_code_patches_are_not_used_for_botw(self):
        [both] = self.add(self.source('Both', {'Actor/Pack/Weapon_Sword_001.sbactorpack': b'x'}, '@nsobid-00\n'))
        v = mod_library.validation(self.library, both)
        self.assertEqual((v.valid, v.code, mod_code.includable(v)), (True, 'not_for_game', False))
        self.assertRaises(ValueError, mod_code.set_code, self.library, both, True, v)
        [code_only] = self.add(self.source('Code', {}, '@nsobid-00\n'))
        v = mod_library.validation(self.library, code_only)
        self.assertEqual((v.valid, v.reason, v.code), (False, 'no_romfs', 'not_for_game'))


class Packages(Base):
    def refused(self, source):
        with self.assertRaises(mod_find.Refused) as caught:
            self.add(source)
        self.assertEqual(self.library.state['order'], [])
        return caught.exception.reason

    def test_bcml_and_tkmm_packages_are_refused_in_words(self):
        bnp = self.tmp / 'Infinite Running.bnp'
        with zipfile.ZipFile(bnp, 'w') as archive:
            archive.writestr('info.json', '{}')
        self.assertEqual(self.refused(bnp), 'bcml_package')
        extracted = self.source('extracted', {})
        (extracted / '01007EF00011E000/romfs/Actor/Pack').mkdir(parents=True)
        (extracted / '01007EF00011E000/romfs/Actor/Pack/Weapon_Sword_001.sbactorpack').write_bytes(b'y')
        (extracted / 'logs').mkdir()
        (extracted / 'info.json').write_text('{}')
        self.assertEqual(self.refused(extracted), 'bcml_package')
        holder = self.tmp / 'downloads'
        holder.mkdir()
        shutil.copy(bnp, holder)
        self.assertEqual(self.refused(holder), 'bcml_package')
        tkcl = self.tmp / 'pack.tkcl'
        tkcl.write_bytes(b'tkmm')
        self.assertEqual(self.refused(tkcl), 'tkmm_package')
        self.assertIn('BCML', tab_mods.say('refused.bcml_package', detail='Infinite Running.bnp'))


class Page(unittest.TestCase):
    """What the Mods page (tools/builder_jobs/tab_mods.view) shows of a BotW state."""

    def setUp(self):
        env = patch.dict(os.environ, MNG_LANG='en')
        env.start()
        self.addCleanup(env.stop)

    def state(self, *mods, **fields):
        return ModsState('botw', 'Breath of the Wild', '1.9.0', Path('/A.app'), mods=mods, **fields)

    def test_no_code_switch_for_botw_and_the_reason_in_words(self):
        v = Validation(True, '', 1, 0, code='not_for_game')
        [mod] = tab_mods.view(self.state(Mod('a', 'Sword', False, v)))['mods']
        self.assertIsNone(mod['code'])
        self.assertIn(tab_mods.say('code.not_for_game'), mod['details'])

    def test_not_applied_is_said(self):                                  # S7.3 N2
        self.assertEqual(tab_mods.view(self.state(not_applied=True))['not_applied'], tab_mods.say('not_applied'))
        self.assertEqual(tab_mods.view(self.state())['not_applied'], '')


if __name__ == '__main__':
    unittest.main()
