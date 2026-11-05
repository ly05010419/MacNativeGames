"""The build's mod helpers: it merges a mod's main patches, chains its heap table into the 2K overlay, names a mod file
the app replaces itself, and - when the player asked for it with --install-mod-romfs - replaces the version's mod folder
with the mod's RomFS files and writes its manifest (scripts/make_mod_manifest.py, run for real against a small fake
RomFS)."""
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
import build_app             # noqa: E402
import mod_folder            # noqa: E402

HEAPDEF = build_app.HEAPDEF


class ModOverlays(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.tree = self.root/'work/totk'
        self.mod = self.root/'The Second Quest 1.4.3'
        (self.mod/'exefs').mkdir(parents=True)
        (self.mod/'romfs').mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def overlay(self, folder, *paths):
        directory = self.tree/f'extracted/v143/{folder}'
        directory.mkdir(parents=True, exist_ok=True)
        (directory/'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256\n'
                                              + ''.join(f'{p}\t1\t00\t01\n' for p in paths))

    def mod_romfs(self, *paths):
        for path in paths:
            file = self.mod/'romfs'/path
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(b'x')

    def test_a_mod_without_a_heap_table_has_nothing_to_chain(self):
        self.assertIsNone(build_app.mod_heapdef(None))
        self.assertIsNone(build_app.mod_heapdef(self.mod))

    def test_the_mods_heap_table_is_found(self):
        self.mod_romfs(HEAPDEF)
        self.assertEqual(build_app.mod_heapdef(self.mod), self.mod/'romfs'/HEAPDEF)

    def test_the_heap_table_is_chained_not_a_clash(self):
        self.mod_romfs(HEAPDEF, 'Banc/MainField/A-1_Dynamic.bcett.byml.zs')
        self.overlay('romfs-patches', 'UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs')
        self.overlay('romfs-patches-out/2560x1440', HEAPDEF)
        build_app.check_mod_overlays(self.tree, self.mod, 'v143')      # no exception

    def test_a_mod_file_the_app_replaces_is_named_not_refused(self):
        # the game-over flow is one of the app's own overlays that cannot be merged: the manifest leaves it out
        self.mod_romfs('Event/EventFlow/DmF_SY_GameOver.bfevfl.zs')
        self.overlay('romfs-patches', 'Event/EventFlow/DmF_SY_GameOver.bfevfl.zs')
        sayings = []
        build_app.say = sayings.append
        clashes = build_app.check_mod_overlays(self.tree, self.mod, 'v143')
        self.assertEqual(clashes, ['Event/EventFlow/DmF_SY_GameOver.bfevfl.zs'])
        self.assertIn('a player\'s manifest leaves it out', sayings[0])

    def test_the_ui_archive_is_merged_rather_than_left_out(self):
        # #73: the build's quick-menu edit goes into the mod's own archive, so the build says merged, not left out
        archive = 'UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs'
        self.mod_romfs(archive)
        self.overlay('romfs-patches', archive)
        sayings = []
        build_app.say = sayings.append
        self.assertEqual(build_app.check_mod_overlays(self.tree, self.mod, 'v143'), [])
        self.assertIn("merged into the mod's copy", sayings[0])
        self.assertIn('#73', sayings[0])

    def test_a_mod_without_romfs_is_not_checked(self):
        build_app.check_mod_overlays(self.tree, None, 'v143')
        (self.mod/'romfs').rmdir()
        build_app.check_mod_overlays(self.tree, self.mod, 'v143')


ROMFS_FILES = {'Pack/A.pack.zs': b'original a', 'Sound/B.bwav': b'original b'}


def fake_romfs(folder):
    """romfs.bin and runtime-index.bin in the layout scripts/prepare_romfs_backend.py writes."""
    image, rows, strings = bytearray(), bytearray(), bytearray()
    for path in sorted(ROMFS_FILES):
        name = path.encode() + b'\0'
        rows += struct.pack('<QQII', len(image), len(ROMFS_FILES[path]), len(strings), len(name))
        image += ROMFS_FILES[path]
        strings += name
    folder.mkdir(parents=True, exist_ok=True)
    (folder/'romfs.bin').write_bytes(image)
    (folder/'runtime-index.bin').write_bytes(
        struct.pack('<8sQQ', b'TOTKRFS1', len(ROMFS_FILES), 24 + len(rows)) + rows + strings)


class ModInstall(unittest.TestCase):
    """--install-mod-romfs (scripts/mod_folder.py): the player's folder for the version is replaced whole by the mod's
    RomFS files, and its manifest is written against the app the build just made."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.logs = self.root/'logs'
        self.logs.mkdir()
        self.app = self.root/'Tears of the Kingdom.app'
        fake_romfs(self.app/'Contents/Resources/totk/extracted/v143/romfs')
        self.mods = self.root/'mods'
        self.mod = self.root/'Mod'
        (self.mod/'exefs').mkdir(parents=True)                 # --mod's usual layout
        self.said = []
        self.saved = mod_folder.mods_folder
        mod_folder.mods_folder = lambda version: self.mods/version     # never touch the real player folder

    def tearDown(self):
        mod_folder.mods_folder = self.saved
        self.tmp.cleanup()

    def add(self, relative, data=b'x'):
        file = self.mod/'romfs'/relative
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)
        return file

    def install(self):
        return mod_folder.install(self.mod, 'v143', self.app, sys.executable, ROOT/'games/totk',
                                     say=self.said.append)

    def manifest(self, folder):
        lines = (folder/'manifest.tsv').read_text().splitlines()
        return {line.split('\t')[0]: line.split('\t') for line in lines if not line.startswith('#')}

    def test_the_mods_romfs_files_replace_the_folder_and_its_manifest_is_written(self):
        self.add('Pack/A.pack.zs', b'replaced a')
        self.add('New/C.dat', b'added c')
        installed = self.install()
        self.assertEqual(installed.folder, self.mods/'v143')
        self.assertEqual((installed.folder/'Pack/A.pack.zs').read_bytes(), b'replaced a')
        self.assertEqual((installed.folder/'New/C.dat').read_bytes(), b'added c')
        entries = self.manifest(installed.folder)
        self.assertEqual(sorted(entries), ['New/C.dat', 'Pack/A.pack.zs'])
        self.assertEqual(entries['New/C.dat'][3], '-')                 # a file the RomFS lacks
        self.assertNotEqual(entries['Pack/A.pack.zs'][3], '-')         # the original's hash is bound
        self.assertIn('2 files (1 replaced, 1 added)', installed.manifest[-1])
        # what is printed names the player's folder, not the copy it was written in
        self.assertIn(str(installed.folder/'manifest.tsv'), installed.manifest[-1])
        self.assertNotIn('.new', '\n'.join(installed.manifest))

    def test_what_the_folder_held_is_kept_in_a_backup_next_to_it(self):
        dest = self.mods/'v143'
        (dest/'Old').mkdir(parents=True)
        (dest/'Old/Gone.zs').write_bytes(b'old')
        (dest/'notes.txt').write_bytes(b'mine')                        # the player's own file: kept with the rest
        (dest/'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256\nOld/Gone.zs\t3\t00\t01\n')
        self.add('Pack/A.pack.zs', b'replaced a')
        installed = self.install()
        self.assertEqual(sorted(p.name for p in dest.iterdir()), ['Pack', 'manifest.tsv'])
        backup = installed.backup
        self.assertEqual(backup.parent, self.mods)
        self.assertTrue(backup.name.startswith('v143.backup-'), backup.name)
        self.assertEqual((backup/'Old/Gone.zs').read_bytes(), b'old')
        self.assertEqual((backup/'notes.txt').read_bytes(), b'mine')
        self.assertIn('Old/Gone.zs', (backup/'manifest.tsv').read_text())
        self.assertTrue(any(str(backup) in line for line in self.said), self.said)

    def test_a_second_install_keeps_the_first_backup(self):
        dest = self.mods/'v143'
        dest.mkdir(parents=True)
        (dest/'notes.txt').write_bytes(b'mine')
        self.add('Pack/A.pack.zs', b'replaced a')
        first = self.install().backup
        second = self.install().backup                                  # within the same second: another name
        self.assertNotEqual(first, second)
        self.assertEqual((first/'notes.txt').read_bytes(), b'mine')
        self.assertEqual(self.manifest(second).keys(), {'Pack/A.pack.zs'})   # the first install's folder, kept too

    def test_an_empty_folder_leaves_no_backup(self):
        (self.mods/'v143').mkdir(parents=True)
        self.add('Pack/A.pack.zs', b'replaced a')
        self.assertIsNone(self.install().backup)
        self.assertEqual(sorted(p.name for p in self.mods.iterdir()), ['v143'])

    def test_a_misspelt_path_stops_before_the_folder_is_touched(self):
        dest = self.mods/'v143'
        dest.mkdir(parents=True)
        (dest/'notes.txt').write_bytes(b'mine')
        self.add('pack/a.pack.zs', b'replaced a')                      # the RomFS has Pack/A.pack.zs
        with self.assertRaises(mod_folder.InstallError) as raised:
            self.install()
        self.assertIn('not a file of this RomFS: pack/a.pack.zs', raised.exception.output)
        self.assertTrue((dest/'notes.txt').exists())                   # checked before anything was deleted
        self.assertFalse((dest/'Pack').exists())

    def test_a_symlinked_folder_is_refused_not_followed(self):
        real = self.root/'elsewhere'
        real.mkdir()
        (real/'keep.txt').write_bytes(b'x')
        self.mods.mkdir(parents=True)
        (self.mods/'v143').symlink_to(real, target_is_directory=True)
        self.add('Pack/A.pack.zs', b'replaced a')
        with self.assertRaises(mod_folder.InstallError) as raised:
            self.install()
        self.assertIn('is a symlink', str(raised.exception))
        self.assertTrue((real/'keep.txt').exists())

    def test_a_symlink_inside_the_folder_moves_to_the_backup_and_its_target_stays(self):
        dest = self.mods/'v143'
        dest.mkdir(parents=True)
        outside = self.root/'outside.txt'
        outside.write_bytes(b'x')
        (dest/'link.txt').symlink_to(outside)
        self.add('Pack/A.pack.zs', b'replaced a')
        installed = self.install()
        self.assertTrue(outside.exists())
        self.assertFalse((dest/'link.txt').exists())
        self.assertTrue((installed.backup/'link.txt').is_symlink())

    def test_a_mod_file_the_app_replaces_is_copied_but_left_out_of_the_manifest(self):
        overlays = self.app/'Contents/Resources/totk/extracted/v143/romfs-patches'
        overlays.mkdir(parents=True)
        (overlays/'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256\nUI/C.bfres\t1\t00\t01\n')
        self.add('UI/C.bfres', b'the mod version')
        self.add('Pack/A.pack.zs', b'replaced a')
        installed = self.install()
        self.assertTrue((installed.folder/'UI/C.bfres').is_file())      # copied with the rest
        self.assertNotIn('UI/C.bfres', self.manifest(installed.folder))  # the app's own version is the one loaded
        self.assertTrue(any('left out (the app replaces it too' in line for line in installed.manifest),
                        installed.manifest)

    def test_a_mod_that_would_replace_nothing_leaves_the_folder_alone(self):
        # every file the app replaces itself: the game would load none of them, so there is no manifest to write - and
        # the player's folder must not be emptied for it (the mod folder is checked, and looked at, first)
        overlays = self.app/'Contents/Resources/totk/extracted/v143/romfs-patches'
        overlays.mkdir(parents=True)
        (overlays/'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256\nUI/C.bfres\t1\t00\t01\n')
        self.add('UI/C.bfres', b'the mod version')
        dest = self.mods/'v143'
        dest.mkdir(parents=True)
        (dest/'notes.txt').write_bytes(b'mine')
        self.assertIsNone(self.install())
        self.assertEqual(sorted(p.name for p in dest.iterdir()), ['notes.txt'])     # exactly as it was
        self.assertIn('would replace or add no file the game loads', self.said[-1])
        self.assertFalse((dest/'manifest.tsv').exists())

    def test_a_failed_copy_leaves_the_folder_as_it_was(self):
        # a full disk or a permission error while the new copy is built must not cost the player their mods, and must
        # not come out as a traceback: the copy is built next to the folder and only then swapped in
        dest = self.mods/'v143'
        dest.mkdir(parents=True)
        (dest/'notes.txt').write_bytes(b'mine')
        self.add('Pack/A.pack.zs', b'replaced a')
        saved = mod_folder.copy_tree

        def run_out_of_space(source, target):
            raise OSError(28, 'No space left on device', str(target))

        mod_folder.copy_tree = run_out_of_space
        try:
            with self.assertRaises(mod_folder.InstallError) as raised:
                self.install()
        finally:
            mod_folder.copy_tree = saved
        self.assertIn('holds what it held before', str(raised.exception))
        self.assertEqual(sorted(p.name for p in dest.iterdir()), ['notes.txt'])
        self.assertFalse((dest/'manifest.tsv').exists())

    def test_nothing_is_left_next_to_the_folder_after_a_clean_install(self):
        self.add('Pack/A.pack.zs', b'replaced a')
        installed = self.install()
        self.assertEqual(sorted(p.name for p in self.mods.iterdir()), ['v143'])
        self.assertEqual(sorted(p.name for p in installed.folder.iterdir()), ['Pack', 'manifest.tsv'])

    def test_a_mod_without_romfs_or_with_an_empty_one_leaves_the_folder_alone(self):
        self.assertIsNone(self.install())
        self.assertFalse((self.mods/'v143').exists())
        self.assertIn('has no romfs/', self.said[0])
        (self.mod/'romfs').mkdir()
        self.assertIsNone(self.install())
        self.assertIn('holds no files', self.said[1])
        self.assertFalse((self.mods/'v143').exists())


class TheOutputOverlayRecord(unittest.TestCase):
    """The 2K output overlay says what it was built from, so a --mod build's heap sizes do not stay in a later build that
    passes no --mod (scripts/make_output_res_overlay.py writes overlay.json, build_app.py reads it)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.tree = ROOT/'games/totk'
        self.overlay = self.root/'extracted/v0/romfs-patches-out/2560x1440'
        self.heapdef = self.root/'HeapDef.byml.zs'
        self.heapdef.write_bytes(b'a mod heap table')

    def tearDown(self):
        self.tmp.cleanup()

    def build(self, **record):
        self.overlay.mkdir(parents=True, exist_ok=True)
        (self.overlay/'manifest.tsv').write_text(f'# path\tsize\tsha256\toriginal_sha256\n{HEAPDEF}\t1\t00\t01\n')
        if record:
            (self.overlay/'overlay.json').write_text(json.dumps(record))
        return build_app.overlay_stale(self.overlay, None, self.tree)

    def script(self):
        return hashlib.sha256((self.tree/'scripts/make_output_res_overlay.py').read_bytes()).hexdigest()

    def test_no_overlay_yet_is_built(self):
        self.assertEqual(build_app.overlay_stale(self.overlay, None, self.tree), 'missing')

    def test_an_overlay_without_a_record_is_built_again(self):
        self.assertEqual(self.build(), 'no record of what it was built from')     # a build from before the record

    def test_the_plain_overlay_is_current(self):
        self.assertEqual(self.build(script_sha256=self.script(), heapdef_sha256=None), '')

    def test_a_mod_builds_heap_table_does_not_stay_in_a_later_plain_build(self):
        modded = hashlib.sha256(self.heapdef.read_bytes()).hexdigest()
        self.assertEqual(self.build(script_sha256=self.script(), heapdef_sha256=modded), 'built from another heap table')

    def test_the_same_mod_overlay_is_reused(self):
        self.overlay.mkdir(parents=True)
        (self.overlay/'manifest.tsv').write_text('# path\n')
        (self.overlay/'overlay.json').write_text(json.dumps(dict(
            script_sha256=self.script(), heapdef_sha256=hashlib.sha256(self.heapdef.read_bytes()).hexdigest())))
        self.assertEqual(build_app.overlay_stale(self.overlay, self.heapdef, self.tree), '')

    def test_another_script_version_builds_again(self):
        self.assertEqual(self.build(script_sha256='0'*64, heapdef_sha256=None),
                         'built by another version of scripts/make_output_res_overlay.py')


class BotwCodeMerge(unittest.TestCase):
    """step_code_botw with a mod (Breath of the Wild 1.9.0): the merged main, not the retail one, goes to the rewriter, the
    other two modules and the retail ExeFS stay as they were, and the report binds the loader to the merged rewrite."""

    NAMES = ('main', 'main.npdm', 'rtld', 'sdk', 'subsdk0')

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.tree = self.root/'work/totk'
        exefs = self.tree/'extracted/botw190/exefs'
        exefs.mkdir(parents=True)
        for name in self.NAMES:
            (exefs/name).write_bytes(name.encode())
        sha = lambda data: hashlib.sha256(data).hexdigest()
        self.expected = dict(display_version='1.9.0', exefs_sha256={name: sha(name.encode()) for name in self.NAMES},
                             rewritten_sha256={f'{name}.patched.bin': sha(f'rewritten {name}'.encode())
                                               for name in ('main', 'sdk', 'subsdk0')})
        self.mod = self.root/'Mod'
        (self.mod/'exefs').mkdir(parents=True)
        self.rewrites, self.merges = [], []

    def merge(self, tree, mod, version, logs):
        self.merges.append((mod, version))
        staged = tree/f'extracted/{version}/mod-exefs'
        staged.mkdir(parents=True, exist_ok=True)
        (staged/'main').write_bytes(b'patched main')
        return dict(folder=str(mod), folder_sha256='f', build_id='b', patches=[dict(edit_count=1)], edit_bytes=4,
                    reserved_sites=7, retail_main_sha256='r', patched_main_sha256='p' * 64, rewritten_main_sha256=None,
                    rewriter_sha256='w', notes=[]), tree/f'extracted/{version}/mod-exefs.json'

    def run_cmd(self, command, log, cwd=None, env=None, quiet=True):
        source, out, name = command[2], Path(command[3]), command[4]
        self.rewrites.append((name, Path(source).relative_to(self.tree).as_posix()))
        text = b'rewritten patched main' if Path(source).name == 'main' and 'mod-exefs' in str(source) else f'rewritten {name}'.encode()
        (out/f'{name}.patched.bin').write_bytes(text)

    def step(self, mod):
        patches = (mock.patch.dict(build_app.VERSIONS, {'botw190': self.expected}),
                   mock.patch.object(build_app, 'source_matches', return_value=True),
                   mock.patch.object(build_app, 'merge_mod_exefs', side_effect=self.merge),
                   mock.patch.object(build_app, 'run', side_effect=self.run_cmd),
                   mock.patch.object(build_app, 'say'))
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)
        (self.tree/'extracted/botw190/elf').mkdir(parents=True, exist_ok=True)
        (self.tree/'extracted/botw190/elf/main-inventory.json').write_text('{}')
        build_app.step_code_botw(self.tree, self.root/'botw', self.root/'update.nsp', self.root/'keys', {}, self.root/'logs',
                                 version='botw190', mod=mod)
        return json.loads((self.tree/'analysis/botw190-port/phase12-validation.json').read_text())

    def test_the_merged_main_is_what_the_rewriter_reads(self):
        report = self.step(self.mod)
        self.assertEqual(self.merges, [(self.mod, 'botw190')])
        self.assertIn(('main', 'extracted/botw190/mod-exefs/main'), self.rewrites)
        self.assertIn(('sdk', 'extracted/botw190/exefs/sdk'), self.rewrites)      # the other modules stay the retail ones
        self.assertIn(('subsdk0', 'extracted/botw190/exefs/subsdk0'), self.rewrites)

    def test_the_report_binds_the_loader_to_the_merged_rewrite(self):
        report = self.step(self.mod)
        files = report['module_file_sha256']
        merged = hashlib.sha256(b'rewritten patched main').hexdigest()
        self.assertEqual(files['rewritten/botw190/main.patched.bin'], merged)
        self.assertEqual(files['rewritten/botw190/sdk.patched.bin'], self.expected['rewritten_sha256']['sdk.patched.bin'])
        self.assertEqual(files['extracted/botw190/exefs/main'], self.expected['exefs_sha256']['main'])    # retail, as prepare_runtime.py asserts
        self.assertEqual(report['mod_exefs']['rewritten_main_sha256'], merged)
        self.assertIn('patches of', report['scope'])

    def test_without_a_mod_nothing_is_merged_and_the_verified_rewrite_is_checked(self):
        report = self.step(None)
        self.assertEqual(self.merges, [])
        self.assertIn(('main', 'extracted/botw190/exefs/main'), self.rewrites)
        self.assertNotIn('mod_exefs', report)
        self.assertEqual(report['module_file_sha256']['rewritten/botw190/main.patched.bin'],
                         self.expected['rewritten_sha256']['main.patched.bin'])

    def test_a_merged_rewrite_that_is_already_there_is_kept(self):
        self.step(self.mod)
        first = list(self.rewrites)
        previous = self.merge
        self.merge = lambda tree, mod, version, logs: (lambda m, p: (m.update(rewritten_main_sha256=hashlib.sha256(
            b'rewritten patched main').hexdigest()), (m, p))[1])(*previous(tree, mod, version, logs))
        self.rewrites.clear()
        self.step(self.mod)
        self.assertEqual(self.rewrites, [])         # all three images are there with the hashes the report names


if __name__ == '__main__':
    unittest.main()
