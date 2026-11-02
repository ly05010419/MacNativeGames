"""tools/builder/mod_add.py (S5.4 C1-C2; S5.5 C2-C3, B5): mods as they come into the library, a pack of several added
as one mod each, and the Add atomic for any failure. Same base as test_builder_mod_library.py (HOME inside the test
folder, a fake built app); real folders and real archives."""
import subprocess
import unittest
from unittest.mock import patch
import zipfile

from test_builder_mod_library import REPO, Base
from tools.builder import mod_add, mod_archive, mod_find, mod_library


class AsTheyCome(Base):
    def added(self, source, name=None):
        ids = mod_add.add(self.runner, REPO, self.app, 'v143', self.library, source, name)
        return ids, [self.library.path(m) for m in ids]

    def test_one_folder_too_many_and_a_flat_romfs(self):
        nested = self.source('Nested/Nested', {'Pack/A.pack.zs': b'modded a'}).parent
        [mod_id], [copy] = self.added(nested)
        self.assertEqual(((copy / 'romfs/Pack/A.pack.zs').read_bytes(), self.library.state['names'][mod_id]),
                         (b'modded a', 'Nested'))
        flat = self.tmp / 'mods/Flat'
        (flat / 'UI').mkdir(parents=True)
        (flat / 'UI/C.bfres').write_bytes(b'modded c')
        (flat / 'readme.txt').write_text('hi')
        _, [copy] = self.added(flat)
        self.assertEqual(sorted(p.name for p in (copy / 'romfs').iterdir()), ['UI', 'readme.txt'])
        self.assertTrue(mod_library.validation(self.library, 'flat').valid)

    def test_a_zip_with_cheats_beside_and_nothing_left_behind(self):
        archive = self.tmp / 'Stamina.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('Stamina/romfs/Pack/A.pack.zs', b'modded a')
            z.writestr('Stamina/cheats/ABCDEF.txt', b'cheat')
        [mod_id], [copy] = self.added(archive)
        listed = {m.id: m for m in mod_library.mods(self.library)}[mod_id]
        self.assertEqual((mod_id, listed.notes, (copy / 'romfs/Pack/A.pack.zs').is_file()),
                         ('stamina', ('cheats_ignored',), True))
        self.assertFalse((self.library.root / mod_add.UNZIP).exists())

    def test_a_7z_pack_becomes_one_mod_each_all_off(self):                           # C2, C3
        src = self.tmp / 'src'
        for part, path in (('Unbreakable', 'Pack/A.pack.zs'), ('Champion', 'Sound/B.bwav')):
            (src / part / 'romfs' / path).parent.mkdir(parents=True)
            (src / part / 'romfs' / path).write_bytes(b'modded')
        archive = self.tmp / 'unbreakable_v141.7z'
        subprocess.run([mod_archive.TAR, '--format', '7zip', '-cf', str(archive), 'Champion', 'Unbreakable'], cwd=src,
                       check=True, capture_output=True)
        ids, _ = self.added(archive)
        names = [self.library.state['names'][m] for m in ids]
        self.assertEqual((names, self.library.state['enabled']),
                         (['unbreakable_v141 / Champion', 'unbreakable_v141 / Unbreakable'], []))
        self.assertTrue(all(mod_library.validation(self.library, m).valid for m in ids))
        self.assertFalse((self.library.root / mod_add.UNZIP).exists())

    def test_a_failure_on_the_third_leaves_the_library_as_it_was(self):            # B5
        self.add('Kept', {'Pack/A.pack.zs': b'kept'})
        before = mod_library.open_library(self.data / 'TOTK', 'v143', self.load).state
        pack = self.tmp / 'mods/Pack'
        for n in range(4):
            (pack / f'part{n}/romfs/Pack/A.pack.zs').parent.mkdir(parents=True)
            (pack / f'part{n}/romfs/Pack/A.pack.zs').write_bytes(b'x')
        real, calls = mod_library.clone, []

        def clone(runner, source, dest):
            calls.append(dest)
            if len(calls) == 3:
                raise OSError('disk full')
            return real(runner, source, dest)
        with patch.object(mod_library, 'clone', side_effect=clone):
            self.assertRaises(OSError, self.added, pack)
        self.assertEqual(self.library.state, before)
        self.assertEqual(sorted(p.name for p in self.library.root.iterdir() if p.is_dir()), ['kept'])
        self.assertEqual(mod_library.open_library(self.data / 'TOTK', 'v143', self.load).state, before)

    def test_a_refusal_adds_nothing(self):
        other = self.source('0100000000010000', {'Pack/A.pack.zs': b'x'})
        with self.assertRaises(mod_find.Refused) as caught:
            self.added(other)
        self.assertEqual((caught.exception.reason, self.library.state['order']), ('other_game', []))
        (self.tmp / 'bad.zip').write_bytes(b'not a zip')
        self.assertRaises(mod_find.Refused, self.added, self.tmp / 'bad.zip')
        self.assertFalse((self.library.root / mod_add.UNZIP).exists())


if __name__ == '__main__':
    unittest.main()
