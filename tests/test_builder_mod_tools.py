"""tools/builder/mod_tools.py (S5.2): main's mod tools called in-process: support by behaviour (O5), validation of a mod
against a fake built app (F2), the code-part check (F1) and the load folder's re-check (F4). The fake RomFS is built
as tests/test_mod_manifest.py builds it; no game data."""
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from test_mod_manifest import fake_romfs
from tools.builder import mod_tools
from tools.builder.games import BOTW, TOTK

REPO = Path(__file__).resolve().parents[1]
FILES = {'Pack/A.pack.zs': b'original a', 'Sound/B.bwav': b'original b' * 100, 'UI/C.bfres': b'c'}
KEY = mod_tools.main_key(REPO)


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp)
        self.app = self.tmp / 'Tears of the Kingdom.app'
        self.romfs = fake_romfs(self.app / 'Contents/Resources/totk/extracted/v143/romfs', FILES)

    def mod(self, files, name='mod', exefs=None):
        root = self.tmp / name
        for path, data in files.items():
            (root / 'romfs' / path).parent.mkdir(parents=True, exist_ok=True)
            (root / 'romfs' / path).write_bytes(data)
        for path, data in (exefs or {}).items():
            (root / 'exefs').mkdir(parents=True, exist_ok=True)
            (root / 'exefs' / path).write_text(data)
        return root

    def overlay(self, *paths):
        """The app's own replacement of paths in romfs-patches-out (the 2K overlay), which a mod cannot share (unlike
        romfs-patches, whose UI archive main merges with the mod, issue #73)."""
        folder = self.romfs.parent / 'romfs-patches-out/2560x1440'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'manifest.tsv').write_text(''.join(f'{p}\t1\t{"0" * 64}\t{"0" * 64}\n' for p in paths))
        for p in paths:
            (folder / p).parent.mkdir(parents=True, exist_ok=True)
            (folder / p).write_bytes(b'the app')


class Support(Base):
    def test_supported_only_where_main_installs_into_the_games_own_folder(self):
        with patch.dict(os.environ, HOME=str(self.tmp)):
            self.assertTrue(mod_tools.supported(REPO, TOTK, 'v143'))
            self.assertTrue(mod_tools.supported(REPO, BOTW, 'botw190'))       # S9.2: main's mods_folder names BotW
            manifest_tool, folder_tool = mod_tools.tools(REPO)
            totk = self.tmp / 'Library/Application Support/MacNativeGames/TOTK/mods/botw190'
            with patch.object(folder_tool, 'mods_folder', return_value=totk):
                self.assertFalse(mod_tools.supported(REPO, BOTW, 'botw190'))  # the old main: not where BotW looks
            with patch.object(manifest_tool, 'APP_VERSIONS', ('v143',)):
                self.assertFalse(mod_tools.supported(REPO, BOTW, 'botw190'))  # a main whose manifest lacks it

    def test_the_1_4_3_key_is_read_from_the_repository(self):
        self.assertRegex(KEY or '', r'^[0-9a-f]{32}$')


class Validate(Base):
    def test_a_mod_that_replaces_a_file_is_valid(self):
        v = mod_tools.validate(REPO, self.app, 'v143', self.mod({'Pack/A.pack.zs': b'modded a', 'New/D.bin': b'd'}))
        self.assertEqual((v.valid, v.replaced, v.added, v.code), (True, 1, 1, 'none'))

    def test_only_adding_files_is_not_valid(self):
        v = mod_tools.validate(REPO, self.app, 'v143', self.mod({'romfs/Pack/A.pack.zs': b'one level off'}))
        self.assertEqual((v.valid, v.reason, v.replaced), (False, 'only_adds', 0))

    def test_a_misspelt_path_is_not_valid(self):
        v = mod_tools.validate(REPO, self.app, 'v143', self.mod({'pack/A.pack.zs': b'x', 'Sound/B.bwav': b'y'}))
        self.assertEqual((v.valid, v.reason, v.misspelt), (False, 'misspelt', ('pack/A.pack.zs',)))

    def test_everything_taken_by_the_app_is_not_valid_here(self):
        self.overlay('UI/C.bfres')
        v = mod_tools.validate(REPO, self.app, 'v143', self.mod({'UI/C.bfres': b'modded c'}))
        self.assertEqual((v.valid, v.reason, v.taken), (False, 'all_taken', ('UI/C.bfres',)))

    def test_code_only_is_not_a_toggle(self):
        v = mod_tools.validate(REPO, self.app, 'v143', self.mod({}, exefs={'x.pchtxt': f'@nsobid-{KEY}00000000\n'}))
        self.assertEqual((v.valid, v.reason, v.code), (False, 'no_romfs', 'ok'))


class Code(Base):
    def check(self, files):
        self.n = getattr(self, 'n', 0) + 1                              # a fresh mod folder for each case
        return mod_tools.code_check(REPO, 'v143', self.mod({}, name=f'code{self.n}', exefs=files) / 'exefs')

    def test_build_ids(self):
        self.assertEqual(self.check({'a.pchtxt': f'@nsobid-{KEY}12345678\n@enabled\n'}), 'ok')
        self.assertEqual(self.check({'a.pchtxt': f'@nsobid-{KEY}12345678{"0" * 24}\n'}), 'ok')   # padded to 64
        self.assertEqual(self.check({f'{KEY}abcdef01.ips': 'PATCH'}), 'ok')
        self.assertEqual(self.check({'a.pchtxt': '@nsobid-' + 'f' * 40 + '\n'}), 'other_build')
        self.assertEqual(self.check({'main.ips': 'PATCH'}), 'unchecked')
        self.assertEqual(mod_tools.code_check(REPO, 'v143', self.tmp / 'none'), 'none')

    def test_a_whole_main_blocks_the_code_and_other_modules_do_not(self):          # S5.4 R3
        self.assertEqual(self.check({'main': 'NSO0', 'a.pchtxt': f'@nsobid-{KEY}12345678\n'}), 'whole_module')
        self.assertEqual(self.check({'subsdk9': 'NSO0', 'main.npdm': 'x', 'a.pchtxt': f'@nsobid-{KEY}12345678\n'}),
                         'ok')

    def test_the_apps_top_folders(self):                                           # S5.4 C1, O5
        self.assertEqual(mod_tools.top_folders(REPO, self.app, 'v143'), {'Pack', 'Sound', 'UI'})
        self.assertEqual(mod_tools.top_folders(REPO, self.tmp / 'none.app', 'v143'), set())


class Versions(Base):                                                              # S5.5 C1, B4
    VERSIONED = {'Pack/Actor/Player.pack.zs': b'player', 'GameData/GameDataList.Product.140.byml.zs': b'gdl',
                 'System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs': b'rstb'}

    def setUp(self):
        super().setUp()
        self.app = self.tmp / 'Versioned.app'
        fake_romfs(self.app / 'Contents/Resources/totk/extracted/v143/romfs', self.VERSIONED)

    def check(self, files, name):
        return mod_tools.validate(REPO, self.app, 'v143', self.mod(files, name=name))

    def test_a_mod_for_older_versions_is_not_valid_and_says_which(self):
        v = self.check({'Pack/Actor/Player.pack.zs': b'old player', 'GameData/GameDataList.Product.110.byml.zs': b'g',
                        'System/Resource/ResourceSizeTable.Product.121.Nin_NX_NVN.rsizetable.zs': b'r',
                        'System/Resource/ResourceSizeTable.Product.110.rsizetable.zs': b'r'}, 'ambient')
        self.assertEqual((v.valid, v.reason, v.versions, v.rules), (False, 'other_version', ('1.1.0', '1.2.1'), 2))

    def test_a_universal_mod_stays_valid_with_a_note(self):
        v = self.check({'GameData/GameDataList.Product.140.byml.zs': b'new',
                        'GameData/GameDataList.Product.110.byml.zs': b'old', 'Pack/Actor/Player.pack.zs': b'p'},
                       'universal')
        self.assertEqual((v.valid, v.reason, v.versions, v.other_ignored), (True, '', (), True))

    def test_names_alone_cannot_date_a_pack_and_the_apps_own_numbers_are_fine(self):
        for files in ({'Pack/Actor/Player.pack.zs': b'older player'},
                      {'GameData/GameDataList.Product.140.byml.zs': b'new', 'Pack/Actor/Player.pack.zs': b'p'}):
            v = self.check(files, f'mod{len(files)}')
            self.assertEqual((v.valid, v.versions, v.other_ignored, v.rules), (True, (), False, mod_tools.RULES))

    def test_an_unknown_number_is_shown_as_it_is(self):
        self.assertEqual(mod_tools.other_versions(self.mod({'GameData/GameDataList.Product.150.byml.zs': b'x'},
                                                           name='future') / 'romfs',
                                                  ['GameData/GameDataList.Product.140.byml.zs']), (('150',), False))


class Recheck(Base):
    def load(self, files):
        load = self.mod(files, name='load') / 'romfs'
        manifest_tool, _ = mod_tools.tools(REPO)
        (load / 'manifest.tsv').write_text(manifest_tool.manifest_text(mod_tools._found(REPO, self.app, 'v143', load)))
        return load

    def test_an_unchanged_app_needs_nothing_and_a_new_clashing_overlay_does(self):
        load = self.load({'Pack/A.pack.zs': b'modded a', 'Sound/B.bwav': b'modded b'})
        self.assertFalse(mod_tools.recheck(REPO, self.app, 'v143', load))
        self.overlay('Sound/B.bwav')                                    # a newer build replaces a listed path
        self.assertTrue(mod_tools.recheck(REPO, self.app, 'v143', load))

    def test_no_manifest_no_warning(self):
        self.assertFalse(mod_tools.recheck(REPO, self.app, 'v143', self.tmp / 'nothing'))


if __name__ == '__main__':
    unittest.main()
