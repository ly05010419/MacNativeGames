"""The Textures tab's model (S9.2): mod_find's RomFS in any case, the pack as the reserved hidden library mod, Apply
fitting its tables first (main's real mod_folder.install on a fake app, as test_builder_mod_library; the fit itself is
main's script, replaced here: it needs a real game), the heap table through the build's record, and Play's 8 GB mode.
O6 of test_builder_mod_library holds: HOME points into the test folder."""
from pathlib import Path
import shutil
from unittest.mock import patch

from test_builder_mod_library import REPO, Base
from test_mod_manifest import fake_romfs
from tools.builder import (controller_start, controller_textures, mod_code, mod_find, mod_library, texture_fit,
                           textures)
from tools.builder.games import TOTK

RSTB = 'System/Resource/ResourceSizeTable.Product.{}.Nin_NX_NVN.rsizetable.zs'
APP_FILES = {'Pack/A.pack.zs': b'original a', 'TexToGo/Grass.txtg': b'small', RSTB.format(143): b'table 143'}
PACK = {'TexToGo/Grass.txtg': b'a much bigger texture', texture_fit.HEAPDEF: b'pack heaps',
        RSTB.format(121): b'table 121'}


class Pack(Base):
    def setUp(self):
        super().setUp()
        shutil.rmtree(self.app)                         # the base's fake app, with a texture and a size table
        fake_romfs(self.app / 'Contents/Resources/totk/extracted/v143/romfs', APP_FILES)

    def pack(self, name='HD Pack', files=PACK, folder='RomFS'):
        """A pack as one ships: <name>/4kTextures/<folder>/... and a readme."""
        root = self.tmp / 'packs' / name
        for path, data in files.items():
            (root / '4kTextures' / folder / path).parent.mkdir(parents=True, exist_ok=True)
            (root / '4kTextures' / folder / path).write_bytes(data)
        (root / 'README.txt').write_text('read me')
        return root

    def add_pack(self, **kw):
        textures.add(self.runner, REPO, self.app, 'v143', self.library, self.pack(**kw))

    def state(self):
        return textures.tab_state(self.library, self.app)


class Model(Pack):
    def test_romfs_is_found_in_any_case(self):
        for n, folder in enumerate(('RomFS', 'romfs', 'ROMFS')):          # a Mac disk ignores case: n names apart
            with self.subTest(folder=folder):
                [found] = mod_find.find(self.pack(f'Pack {n}', folder=folder), ['TexToGo', 'Pack', 'System'], '')
                self.assertEqual((found.romfs.name, found.name), (folder, '4kTextures'))

    def test_the_pack_is_reserved_hidden_and_after_the_players_mods(self):
        mine = self.add('Mine', {'Pack/A.pack.zs': b'mine'})
        mod_library.set_enabled(self.library, mine, True)
        self.add_pack()
        t = self.state()
        self.assertEqual((t.present, t.name, t.enabled, t.valid, t.files), (True, 'HD Pack', True, True, 2))
        self.assertFalse((self.library.path(textures.mod_library.TEXTURES) / 'romfs' / RSTB.format(121)).exists())
        self.assertEqual([m.id for m in mod_library.mods(self.library)], [mine])        # no row in the Mods tab
        self.assertEqual(self.library.state['order'], [mine, mod_library.TEXTURES])
        mod_library.move(self.library, mine, 1)
        self.assertEqual(self.library.state['order'], [mine, mod_library.TEXTURES])
        self.add_pack(name='Other Pack', files={'TexToGo/Grass.txtg': b'another texture'})
        romfs = self.library.path(mod_library.TEXTURES) / 'romfs'
        self.assertEqual((self.state().name, sorted(p.name for p in romfs.rglob('*') if p.is_file())),
                         ('Other Pack', ['Grass.txtg']))                # replaced whole
        textures.remove(self.library)
        self.assertEqual((self.state().present, self.library.state['order']), (False, [mine]))

    def test_unpruned_the_pack_would_be_refused_as_another_versions(self):
        with patch.object(textures, '_pruner', return_value=lambda folder: None):
            self.add_pack()
        self.assertEqual((self.state().valid, self.state().reason), (False, 'other_version'))

    def test_apply_fits_the_tables_before_the_manifest_and_stops_when_it_fails(self):
        self.add_pack()
        seen = []
        def fit(repo, app, version, romfs, cancel=None):
            seen.append((Path(romfs).name, Path(romfs).parent.name, version, (Path(romfs) / RSTB.format(143)).exists()))
            (Path(romfs) / RSTB.format(143)).write_bytes(b'fitted')     # as the script writes the app's table
        with patch.object(texture_fit, 'fit', side_effect=fit):
            r = self.apply()
        self.assertEqual((r.ok, seen), (True, [('romfs', '.compose', 'v143', False)]))
        self.assertEqual((self.load / RSTB.format(143)).read_bytes(), b'fitted')
        before = sorted(p.name for p in self.load.rglob('*'))
        with patch.object(texture_fit, 'fit', return_value=('no such table',)):
            r = self.apply()
        self.assertEqual((r.ok, r.reason, r.lines), (False, 'textures_failed', ('no such table',)))
        self.assertEqual(sorted(p.name for p in self.load.rglob('*')), before)     # the game's folder untouched
        textures.set_on(self.library, False)
        mine = self.add('Mine', {'Pack/A.pack.zs': b'mine'})
        mod_library.set_enabled(self.library, mine, True)
        with patch.object(texture_fit, 'fit') as fit:
            self.assertTrue(self.apply().ok)
        fit.assert_not_called()                         # the pack off: no fit

    def test_the_heap_rides_the_build_record_and_play_follows_it(self):
        self.add_pack()
        sha = textures.wanted_heap(self.library)
        fitted = self.tmp / 'fitted.byml.zs'
        fitted.write_bytes(b'fitted heaps')
        self.assertEqual((self.state().heap_pending, textures.play_env(self.library, self.app)), (True, {}))
        folder, record = mod_code.compose(self.library, (), (fitted, sha))
        self.assertEqual(((folder / 'romfs' / texture_fit.HEAPDEF).read_bytes(), record),
                         (b'fitted heaps', ((mod_code.HEAP, (('heapdef', sha),)),)))
        mod_code.write_record(self.library.root, record, self.app)
        t = self.state()
        self.assertEqual((t.heap_pending, t.built_with_heap, mod_code.pending(self.library, self.app)),
                         (False, True, False))           # the heap is no code: no code-pending line
        self.assertEqual(textures.play_env(self.library, self.app), {'TOTK_DRAM_8GB': '1'})
        self.assertEqual(textures.play_env(self.library, self.tmp / 'Other.app'), {})
        textures.set_on(self.library, False)
        t = self.state()
        self.assertEqual((t.heap_pending, t.built_with_heap), (True, True))     # build again to drop the heaps
        self.assertEqual(textures.play_env(self.library, self.app), {'TOTK_DRAM_8GB': '1'})
        self.assertEqual(mod_code.compose(self.library, (), None), (None, ()))

    def test_the_builds_heap_is_fitted_to_the_existing_app(self):
        self.add_pack()
        with patch.object(texture_fit, 'heap', return_value=Path('/fitted')) as heap:
            self.assertEqual(textures.build_heap(REPO, self.library, self.app, 'v143'),
                             (Path('/fitted'), textures.wanted_heap(self.library)))
        self.assertEqual(heap.call_args.args[:4], (REPO, self.app, 'v143', textures.pack_heapdef(self.library)))
        self.assertRaises(RuntimeError, textures.build_heap, REPO, self.library, self.tmp / 'gone.app', 'v143')
        textures.set_on(self.library, False)
        self.assertIsNone(textures.build_heap(REPO, self.library, self.tmp / 'gone.app', 'v143'))

    def test_only_a_totk_build_with_its_update_carries_the_heap(self):
        self.add_pack()
        fields = (('game', '/t.xci'), ('keys', '/k'), ('out', str(self.app)))
        self.assertIsNone(controller_textures.heap_for_build(REPO, self.library, ('totk', fields)))
        with_update = fields + (('update', '/u.nsp'),)
        with patch.object(textures, 'build_heap', return_value='heap') as build_heap:
            self.assertEqual(controller_textures.heap_for_build(REPO, self.library, ('totk', with_update))(), 'heap')
        self.assertEqual(build_heap.call_args.args[2], self.app)
        self.assertIsNone(controller_textures.heap_for_build(REPO, self.library, ('botw', with_update)))
        textures.set_on(self.library, False)
        self.assertIsNone(controller_textures.heap_for_build(REPO, self.library, ('totk', with_update)))

    def test_play_passes_the_8gb_mode_from_the_library(self):
        self.add_pack()
        mod_code.write_record(self.library.root, ((mod_code.HEAP, (('heapdef', 'x'),)),), self.app)
        with patch.object(controller_start.start_options, 'play', return_value='played') as play:
            paths = controller_start.start_options.StartPaths(self.tmp / 'start.json', self.data)
            self.assertEqual(controller_start._play('runner', TOTK, self.app, paths, 'yes', REPO), 'played')
        self.assertEqual(play.call_args.args[-1], {'TOTK_DRAM_8GB': '1'})
