"""make_mod_manifest.py: the manifest of a player's RomFS mod, checked against a small fake RomFS (no game data)."""
import hashlib
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
SCRIPT = ROOT/'games/totk/scripts/make_mod_manifest.py'
import make_mod_manifest      # noqa: E402  the writer test calls the seam make_button_icons.py calls
HAS_ZSTD = importlib.util.find_spec('compression.zstd') is not None   # Python 3.14
FILES = {'Pack/A.pack.zs': b'original a', 'Sound/B.bwav': b'original b' * 100, 'UI/C.bfres': b'c'}


def fake_romfs(folder, files=FILES):
    """romfs.bin and runtime-index.bin in the layout scripts/prepare_romfs_backend.py writes."""
    folder.mkdir(parents=True)
    image, rows, strings = bytearray(), bytearray(), bytearray()
    for path in sorted(files):
        name = path.encode() + b'\0'
        rows += struct.pack('<QQII', len(image), len(files[path]), len(strings), len(name))
        image += files[path]
        strings += name
    (folder/'romfs.bin').write_bytes(image)
    (folder/'runtime-index.bin').write_bytes(struct.pack('<8sQQ', b'TOTKRFS1', len(files), 24 + len(rows)) + rows + strings)
    return folder


def sarc(members):
    """A little-endian SARC holding `members` (name -> bytes), the layout scripts/make_romfs_patches.py reads."""
    names, table, data = bytearray(), bytearray(), bytearray()
    for name, blob in members.items():
        while len(data) % 8: data += b'\0'
        table += struct.pack('<4I', 0, 0x01000000 | len(names) // 4, len(data), len(data) + len(blob))
        names += name.encode() + b'\0'
        while len(names) % 4: names += b'\0'
        data += blob
    head = b'SFAT' + struct.pack('<HHI', 12, len(members), 0x65) + bytes(table) + b'SFNT' + struct.pack('<HH', 8, 0) + bytes(names)
    start = 0x14 + len(head); start += -start % 8
    out = bytearray(b'SARC' + struct.pack('<H', 0x14) + b'\xff\xfe' + struct.pack('<IIHH', 0, start, 0x100, 0) + head)
    out += b'\0' * (start - len(out)) + data
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def bflan(panes, frame=1.0):
    """A BFLAN with one pai1 section: each pane has one FLVI (visibility) step track with the keys (0.0, 0) (frame, 1)."""
    entries = b''
    offsets = []
    for pane in panes:
        entry = pane.encode().ljust(28, b'\0') + bytes([1, 0, 0, 0]) + struct.pack('<I', 36)
        track = b'FLVI' + bytes([1, 0, 0, 0]) + struct.pack('<I', 12)
        group = bytes([0, 0, 1, 0]) + struct.pack('<HH', 2, 0) + struct.pack('<I', 12)
        keys = struct.pack('<fHxx', 0.0, 0) + struct.pack('<fHxx', frame, 1)
        offsets.append(20 + 4 * len(panes) + len(entries))
        entries += entry + track + group + keys
    body = struct.pack('<HI', len(panes), 20) + b''.join(struct.pack('<I', o) for o in offsets) + entries   # count at +14, table at +20
    section = b'pai1' + struct.pack('<I', 8 + 6 + len(body)) + b'\0' * 6 + body
    return b'FLAN\xff\xfe' + b'\0' * 10 + struct.pack('<H', 1) + b'\0' * 2 + section


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)


class ModManifest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.romfs = fake_romfs(self.root/'romfs')
        self.mod = self.root/'mods/v143'

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, path, data):
        (self.mod/path).parent.mkdir(parents=True, exist_ok=True)
        (self.mod/path).write_bytes(data)

    def test_lists_replaced_files_with_both_hashes(self):
        self.put('Sound/B.bwav', b'dubbed')
        self.put('UI/C.bfres', b'c')   # same as the original: left out
        self.put('.DS_Store', b'finder')
        result = run(self.mod, '--romfs', self.romfs)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [l for l in (self.mod/'manifest.tsv').read_text().splitlines() if not l.startswith('#')]
        self.assertEqual(lines, ['\t'.join(['Sound/B.bwav', '6', hashlib.sha256(b'dubbed').hexdigest(),
                                            hashlib.sha256(FILES['Sound/B.bwav']).hexdigest()])])
        self.assertIn('(same as the original): UI/C.bfres', result.stdout)

    def test_misspelt_path_writes_nothing(self):
        self.put('Sound/b.bwav', b'wrong case')
        result = run(self.mod, '--romfs', self.romfs)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not a file of this RomFS: Sound/b.bwav (it has Sound/B.bwav', result.stderr)
        self.assertFalse((self.mod/'manifest.tsv').exists())

    def test_new_file_is_added(self):   # issue #48
        self.put('Sound/B.bwav', b'dubbed')
        self.put('UI/Tex/New/Gold.bntx.zs', b'new texture')
        result = run(self.mod, '--romfs', self.romfs)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = sorted(l for l in (self.mod/'manifest.tsv').read_text().splitlines() if not l.startswith('#'))
        self.assertEqual(lines[1], '\t'.join(['UI/Tex/New/Gold.bntx.zs', '11', hashlib.sha256(b'new texture').hexdigest(), '-']))
        self.assertIn('(1 replaced, 1 added)', result.stdout)

    def test_app_path_takes_the_version_from_the_folder(self):
        app = self.root/'Tears of the Kingdom.app'
        fake_romfs(app/'Contents/Resources/totk/extracted/v143/romfs')
        self.put('Pack/A.pack.zs', b'modded a')
        result = run(self.mod, '--app', app)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('1 files', result.stdout)

    def test_library_app_reads_the_image_from_game_data(self):   # build_app.py --mac-game-data library
        app = self.root/'Tears of the Kingdom.app'
        (fake_romfs(app/'Contents/Resources/totk/extracted/v143/romfs')/'romfs.bin').unlink()
        home = self.root/'home'
        fake_romfs(home/'Library/Application Support/MacNativeGames/TOTK/GameData/v143')
        self.put('Pack/A.pack.zs', b'modded a')
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.mod), '--app', str(app)], capture_output=True,
                                text=True, env={**os.environ, 'HOME': str(home)})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('1 files', result.stdout)
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.mod), '--app', str(app)], capture_output=True,
                                text=True, env={**os.environ, 'HOME': str(self.root/'nobody')})
        self.assertEqual(result.returncode, 2)
        self.assertIn('GameData/v143/romfs.bin not found', result.stderr)

    def test_library_app_reads_the_image_from_the_folder_the_build_was_given(self):   # --mac-game-data-folder
        import plistlib
        app = self.root/'Tears of the Kingdom.app'
        (fake_romfs(app/'Contents/Resources/totk/extracted/v143/romfs')/'romfs.bin').unlink()
        fake_romfs(self.root/'disk/Games/v143')
        (app/'Contents/Info.plist').write_bytes(plistlib.dumps({'MNGGameDataFolder': str(self.root/'disk/Games')}))
        self.put('Pack/A.pack.zs', b'modded a')
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.mod), '--app', str(app)], capture_output=True,
                                text=True, env={**os.environ, 'HOME': str(self.root/'nobody')})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(make_mod_manifest.app_image_dir(app, 'v143'), self.root/'disk/Games/v143')
        (self.root/'disk/Games/v143/romfs.bin').unlink()   # that disk not connected: the data folder's GameData
        home = self.root/'home'
        fake_romfs(home/'Library/Application Support/MacNativeGames/TOTK/GameData/v143')
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.mod), '--app', str(app)], capture_output=True,
                                text=True, env={**os.environ, 'HOME': str(home)})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_library_app_reads_the_image_from_the_folder_chosen_when_it_asked(self):   # app/launcher.m game_data_ask
        import plistlib
        domain = 'org.macnativegames.test-mod-manifest'
        app = self.root/'Breath of the Wild.app'
        (fake_romfs(app/'Contents/Resources/totk/extracted/botw190/romfs')/'romfs.bin').unlink()
        (app/'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': domain,
                                                                'MNGGameDataFolder': str(self.root/'disk/Games')}))
        fake_romfs(self.root/'disk/Games/botw190')
        fake_romfs(self.root/'picked')   # romfs.bin right in the folder the player chose
        subprocess.run(['defaults', 'write', domain, 'MNGGameDataFolder.botw190', str(self.root/'picked')], check=True)
        self.addCleanup(subprocess.run, ['defaults', 'delete', domain], capture_output=True)
        self.assertEqual(make_mod_manifest.app_image_dir(app, 'botw190'), self.root/'picked')
        (self.root/'picked/romfs.bin').unlink()   # gone: the build's folder
        self.assertEqual(make_mod_manifest.app_image_dir(app, 'botw190'), self.root/'disk/Games/botw190')

    def test_romfs_data_names_the_folder_of_the_image(self):
        index = self.root/'index-only'
        index.mkdir()
        (index/'runtime-index.bin').write_bytes((self.romfs/'runtime-index.bin').read_bytes())
        self.put('Sound/B.bwav', b'dubbed')
        self.assertNotEqual(run(self.mod, '--romfs', index).returncode, 0)
        result = run(self.mod, '--romfs', index, '--romfs-data', self.romfs)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(hashlib.sha256(FILES['Sound/B.bwav']).hexdigest(), (self.mod/'manifest.tsv').read_text())

    def test_botw_app_path_takes_the_version_from_the_folder(self):
        for version in ('botw160', 'botw190'):
            with self.subTest(version):
                app = self.root/version/'Breath of the Wild.app'
                fake_romfs(app/'Contents/Resources/totk/extracted'/version/'romfs')
                self.mod = self.root/version/'mods'/version
                self.put('Pack/A.pack.zs', b'modded a')
                result = run(self.mod, '--app', app)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('1 files', result.stdout)

    def test_botw190_app_with_a_named_version(self):   # a mod folder under any name, with the version passed
        app = self.root/'Breath of the Wild.app'
        romfs = app/'Contents/Resources/totk/extracted/botw190/romfs'
        fake_romfs(self.root/'data')                       # the app holds a link to romfs.bin (build_app.py place_data_bundle)
        romfs.mkdir(parents=True)
        (romfs/'romfs.bin').symlink_to(self.root/'data/romfs.bin')
        (romfs/'runtime-index.bin').write_bytes((self.root/'data/runtime-index.bin').read_bytes())
        self.mod = self.root/'library'
        self.put('Sound/B.bwav', b'dubbed')
        result = run(self.mod, '--app', app, '--version', 'botw190')
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [l for l in (self.mod/'manifest.tsv').read_text().splitlines() if not l.startswith('#')]
        self.assertEqual(lines, ['\t'.join(['Sound/B.bwav', '6', hashlib.sha256(b'dubbed').hexdigest(),
                                            hashlib.sha256(FILES['Sound/B.bwav']).hexdigest()])])



COMMON = 'UI/LayoutArchive/Common.Product.110.Nin_NX_NVN.blarc.zs'
CAPTURE = {'anim/ShortCutBG_00_In.bflan': ('C_CaptureBG_00', 'C_CaptureClearBG_00', 'P_CaptureClearBG_00'),
           'anim/AutoBuilder_00_In.bflan': ('C_CaptureBG_00',)}


@unittest.skipUnless(HAS_ZSTD, 'needs Python 3.14 (compression.zstd)')
class MergeWithBuildPatch(unittest.TestCase):
    """Issue #73: a mod's UI archive that the build's romfs-patches also replaces is merged with the build's edit."""
    def setUp(self):
        import compression.zstd as zstd
        sys.path.insert(0, str(ROOT/'games/totk/scripts'))
        from make_romfs_patches import patch_capture, sarc_members
        self.zstd, self.sarc_members = zstd, sarc_members
        samples = [f'sample {i} '.encode() * (i % 7 + 1) + bytes(range(i % 251)) for i in range(400)]
        self.dictionary = zstd.train_dict(samples, 4096)
        pack = zstd.compress(sarc({'zs.zsdic': self.dictionary.dict_content}))
        self.members = {name: bflan(panes) for name, panes in CAPTURE.items()}
        self.members['timg/__Combined.bntx'] = b'original textures' * 40
        self.original = sarc(self.members)
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.romfs = fake_romfs(root/'romfs', {'Pack/ZsDic.pack.zs': pack, COMMON: self.pack(self.original)})
        build = bytearray(self.original)
        patch_capture(build, sarc_members(build), 0.0)
        self.build = self.pack(bytes(build))
        (root/'romfs-patches/UI/LayoutArchive').mkdir(parents=True)
        (root/'romfs-patches'/COMMON).write_bytes(self.build)
        (root/'romfs-patches/manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256\n' + '\t'.join(
            [COMMON, str(len(self.build)), hashlib.sha256(self.build).hexdigest(), hashlib.sha256(self.pack(self.original)).hexdigest()]) + '\n')
        self.mod = root/'mods/v121'

    def tearDown(self):
        self.tmp.cleanup()

    def pack(self, data):
        return self.zstd.compress(data, level=19, zstd_dict=self.dictionary)

    def member(self, raw, name):
        data = self.zstd.decompress(raw, zstd_dict=self.dictionary)
        lo, hi = self.sarc_members(data)[name]
        return data[lo:hi]

    def put_mod(self, members):
        (self.mod/COMMON).parent.mkdir(parents=True, exist_ok=True)
        mod = self.pack(sarc(members))
        (self.mod/COMMON).write_bytes(mod)
        return mod

    def test_translated_ui_archive_takes_the_build_edit(self):
        mod = self.put_mod({**self.members, 'timg/__Combined.bntx': b'translated textures' * 40})
        result = run(self.mod, '--romfs', self.romfs)
        self.assertEqual(result.returncode, 0, result.stderr)
        merged = (self.mod/COMMON).read_bytes()
        self.assertEqual(self.member(merged, 'timg/__Combined.bntx'), b'translated textures' * 40)
        for name in CAPTURE:
            self.assertEqual(self.member(merged, name), self.member(self.build, name))
        self.assertEqual((self.mod/(COMMON + '.mod-original')).read_bytes(), mod)
        lines = [l for l in (self.mod/'manifest.tsv').read_text().splitlines() if not l.startswith('#')]
        self.assertEqual(lines, ['\t'.join([COMMON, str(len(merged)), hashlib.sha256(merged).hexdigest(),
                                            hashlib.sha256(self.build).hexdigest()])])
        again = run(self.mod, '--romfs', self.romfs)   # a second run merges the kept copy again, the same way
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual((self.mod/COMMON).read_bytes(), merged)

    @unittest.skipUnless(importlib.util.find_spec('fontTools'), 'needs fontTools')
    def test_button_icons_merge_the_common_archive_too(self):
        # make_button_icons.py redraws the button guide in this archive; its manifest used to name the RomFS original,
        # and the game stopped at start: two overlays replaced the same file
        import make_button_icons
        self.put_mod({**self.members, 'timg/__Combined.bntx': b'keyboard textures' * 40})
        index = make_mod_manifest.read_index(self.romfs/'runtime-index.bin')
        merged, found = make_button_icons.write_mod_manifest(self.mod, self.romfs, index)
        self.assertEqual(list(merged), [COMMON])
        data = (self.mod/COMMON).read_bytes()
        self.assertEqual(self.member(data, 'timg/__Combined.bntx'), b'keyboard textures' * 40)
        for name in CAPTURE:
            self.assertEqual(self.member(data, name), self.member(self.build, name))
        self.assertEqual(found.lines, ['\t'.join([COMMON, str(len(data)), hashlib.sha256(data).hexdigest(),
                                                  hashlib.sha256(self.build).hexdigest()])])

    def test_mod_that_changes_a_patched_member_is_refused(self):
        changed = bflan(CAPTURE['anim/AutoBuilder_00_In.bflan'], frame=0.5)
        self.put_mod({**self.members, 'anim/AutoBuilder_00_In.bflan': changed})
        result = run(self.mod, '--romfs', self.romfs)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('the mod changes anim/AutoBuilder_00_In.bflan, which the build patches too', result.stderr)
        self.assertFalse((self.mod/'manifest.tsv').exists())

    def test_original_archive_in_the_mod_is_left_out(self):
        self.put_mod(self.members)
        (self.mod/'Pack').mkdir(parents=True, exist_ok=True)
        result = run(self.mod, '--romfs', self.romfs)
        self.assertNotEqual(result.returncode, 0)   # nothing else in the folder: no manifest to write
        self.assertIn('replaces or adds no file', result.stderr)
        self.assertFalse((self.mod/(COMMON + '.mod-original')).exists())


class TheWriterToolsShare(unittest.TestCase):
    """make_button_icons.py writes a mod's manifest with write_manifest() - it used to unpack manifest()'s return into
    four names, which stopped working when the record grew. This runs the code that caller runs, on the same fake
    RomFS: no game data and no fontTools needed."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.romfs = fake_romfs(self.root/'romfs')
        self.mod = self.root/'mods/v143'

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, path, data):
        (self.mod/path).parent.mkdir(parents=True, exist_ok=True)
        (self.mod/path).write_bytes(data)

    def test_the_record_is_read_by_name_and_the_manifest_is_written(self):
        self.put('Sound/B.bwav', b'dubbed')
        self.put('UI/Tex/New/Gold.bntx.zs', b'new texture')
        found = make_mod_manifest.write_manifest(self.mod, self.romfs)
        self.assertEqual(found._fields, ('lines', 'same', 'added', 'misspelt', 'excluded', 'taken'))
        self.assertEqual(found.misspelt, [])
        self.assertEqual(found.added, ['UI/Tex/New/Gold.bntx.zs'])
        self.assertEqual([line.split('\t')[0] for line in found.lines], ['Sound/B.bwav', 'UI/Tex/New/Gold.bntx.zs'])
        self.assertEqual((self.mod/'manifest.tsv').read_text(), make_mod_manifest.manifest_text(found))
        self.assertTrue((self.mod/'manifest.tsv').read_text().startswith(
            '# path\tsize\tsha256\toriginal_sha256 (generated by scripts/make_mod_manifest.py)\n'))

    def test_a_misspelt_path_writes_no_manifest(self):
        self.put('Sound/b.bwav', b'wrong case')
        found = make_mod_manifest.write_manifest(self.mod, self.romfs)
        self.assertEqual(found.misspelt, [('Sound/b.bwav', 'Sound/B.bwav')])   # the caller names it, as before
        self.assertFalse((self.mod/'manifest.tsv').exists())


if __name__ == '__main__':
    unittest.main()
