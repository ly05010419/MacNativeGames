"""tools/builder/mod_archive.py (S5.4 C2; S5.5 C2, C4): a .zip with zipfile, a .7z through the system's bsdtar (real
archives made here with `tar --format 7zip`), the TKMM package, the limits, and a cancel or a timeout that kills tar's
group (a fake tar that writes its pid and sleeps). Everything in a temporary folder."""
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import zipfile

from tools.builder import mod_archive, mod_find
from tools.builder.mod_find import Refused

TOTK, TOPS = '0100F2C0115B6000', {'Pack', 'UI'}


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp).resolve()
        self.into = self.tmp / 'out'

    def extract(self, archive, cancel=None):
        return mod_archive.extract(archive, self.into, cancel)

    def reason(self, archive, cancel=None):
        with self.assertRaises(Refused) as caught:
            self.extract(archive, cancel)
        return caught.exception.reason

    def nothing_out(self):
        return not self.into.exists() or not any(self.into.rglob('*'))


class Zip(Base):
    def zip(self, entries, name='mod.zip'):
        path = self.tmp / name
        with zipfile.ZipFile(path, 'w') as z:
            for entry, data in entries.items():
                z.writestr(data, b'target') if isinstance(data, zipfile.ZipInfo) else z.writestr(entry, data)
        return path

    def test_a_good_zip_then_found(self):
        out = self.extract(self.zip({'Mod/romfs/Pack/A.pack.zs': b'a', '__MACOSX/Mod/._x': b'fork'}))
        self.assertEqual(mod_find.find(out, TOPS, TOTK)[0].romfs, out / 'Mod/romfs')
        self.assertFalse((out / '__MACOSX').exists())

    def test_unsafe_entries_are_refused(self):
        link = zipfile.ZipInfo('Mod/romfs/link')
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        cases = [('zip_path', {'../evil': b'x'}), ('zip_path', {'/etc/x': zipfile.ZipInfo('/etc/x')}),
                 ('zip_link', {'Mod/romfs/link': link}), ('zip_case', {'Mod/A.txt': b'x', 'Mod/a.txt': b'y'}),
                 ('zip_case', {'Mod/café': b'x', 'Mod/café': b'y'}),         # NFC and NFD: one name
                 ('zip_path', {'Mod/romfs/x': b'file', 'Mod/romfs/X/y': b'below'})]
        for n, (reason, entries) in enumerate(cases):
            self.assertEqual(self.reason(self.zip(entries, f'{n}.zip')), reason)
            self.assertTrue(self.nothing_out(), reason)
        self.assertFalse((self.tmp.parent / 'evil').exists())

    def test_limits_and_kinds(self):
        archive = self.zip({'a': b'x' * 100, 'b': b'y'})
        with patch.object(mod_archive, 'MAX_ENTRIES', 1):
            self.assertEqual(self.reason(archive), 'zip_entries')
        with patch.object(mod_archive, 'MAX_BYTES', 50):
            self.assertEqual(self.reason(archive), 'zip_big')
        with patch.object(mod_archive.shutil, 'disk_usage', return_value=shutil._ntuple_diskusage(1, 1, 0)):
            self.assertEqual(self.reason(archive), 'zip_space')
        for name, reason in (('mod.ace', 'archive_kind'), ('The sword.tkcl', 'tkmm_package')):
            (self.tmp / name).write_bytes(b'x')
            self.assertEqual(self.reason(self.tmp / name), reason)
        (self.tmp / 'bad.zip').write_bytes(b'not a zip')
        self.assertEqual(self.reason(self.tmp / 'bad.zip'), 'zip_bad')


class SevenZip(Base):
    def seven(self, files, links=(), name='mod.7z', top='Mod', flags=()):
        src = self.tmp / f'src-{name}'
        for path, data in files.items():
            (src / path).parent.mkdir(parents=True, exist_ok=True)
            (src / path).write_bytes(data)
        for path, target in links:
            (src / path).parent.mkdir(parents=True, exist_ok=True)
            (src / path).symlink_to(target)
        subprocess.run([mod_archive.TAR, *flags, '--format', '7zip', '-cf', str(self.tmp / name), top], cwd=src,
                       check=True, capture_output=True)
        return self.tmp / name

    def test_a_good_7z_with_a_space_and_mac_forks(self):
        archive = self.seven({'Mod/romfs/Pack/A.pack.zs': b'a' * 1000, 'Mod/with space/read me.txt': b'hi'})
        out = self.extract(archive)
        self.assertEqual(((out / 'Mod/romfs/Pack/A.pack.zs').read_bytes(), mod_find.find(out, TOPS, TOTK)[0].romfs),
                         (b'a' * 1000, out / 'Mod/romfs'))

    def test_a_link_or_dots_are_refused_from_the_listing(self):
        self.assertEqual(self.reason(self.seven({'Mod/romfs/x': b'x'}, [('Mod/link', '/etc/hosts')])), 'zip_link')
        self.assertTrue(self.nothing_out())
        (self.tmp / 'evil').write_bytes(b'x')
        (self.tmp / 'src-dots.7z').mkdir()
        dots = self.seven({}, name='dots.7z', top='../evil', flags=('-P',))
        self.assertEqual(self.reason(dots), 'zip_path')
        self.assertTrue(self.nothing_out())

    def test_the_declared_size_and_the_file_cap(self):                                 # B1, B3
        archive = self.seven({'Mod/big.bin': bytes(3_000_000), 'Mod/small.txt': b'x'})
        with patch.object(mod_archive, 'MAX_BYTES', 1_000_000):
            self.assertEqual(self.reason(archive), 'zip_big')                      # before extracting
        self.assertTrue(self.nothing_out())
        with patch.object(mod_archive, 'FILE_BYTES', 1_000_000):
            self.assertEqual(self.reason(archive), 'zip_big')                      # tar stopped by SIGXFSZ
        with patch.object(mod_archive.shutil, 'disk_usage', return_value=shutil._ntuple_diskusage(1, 1, 0)):
            self.assertEqual(self.reason(archive), 'zip_space')

    def test_names_apfs_would_merge(self):
        for entries in ([('Mod/A', False), ('Mod/a', False)], [('Mod/café', False), ('Mod/café', False)],
                        [('Mod/x', False), ('Mod/x/y', False)], [('a\nb', False)], [('/etc/x', False)]):
            with self.assertRaises(Refused):
                mod_archive._checked(entries)

    def test_a_bad_archive_and_a_missing_tar(self):
        (self.tmp / 'bad.7z').write_bytes(b'not 7z')
        self.assertEqual(self.reason(self.tmp / 'bad.7z'), 'zip_bad')
        with patch.object(mod_archive, 'TAR', str(self.tmp / 'no-tar')):
            self.assertEqual(self.reason(self.tmp / 'bad.7z'), 'zip_bad')


class Stopping(Base):                                                                 # B2
    def fake_tar(self):
        pid_file, tar = self.tmp / 'tar.pid', self.tmp / 'tar'
        tar.write_text(f'#!/bin/sh\necho $$ > "{pid_file}"\nexec sleep 30\n')
        tar.chmod(0o755)
        (self.tmp / 'mod.7z').write_bytes(b'x')
        return pid_file, tar

    def stopped(self, pid_file, cancel, reason):
        start = time.monotonic()
        self.assertEqual(self.reason(self.tmp / 'mod.7z', cancel), reason)
        self.assertLess(time.monotonic() - start, 5)                    # killed: the fake tar sleeps 30 s
        pid = int(pid_file.read_text())
        self.assertRaises(ProcessLookupError, os.kill, pid, 0)          # and its group is gone

    def test_a_cancel_kills_tar(self):
        pid_file, tar = self.fake_tar()
        cancel = threading.Event()
        timer = threading.Timer(0.5, cancel.set)
        self.addCleanup(timer.cancel)
        timer.start()
        with patch.object(mod_archive, 'TAR', str(tar)):
            self.stopped(pid_file, cancel, 'cancelled')

    def test_a_timeout_kills_tar(self):
        pid_file, tar = self.fake_tar()
        with patch.object(mod_archive, 'TAR', str(tar)), patch.object(mod_archive, 'LIST_S', 0.5):
            self.stopped(pid_file, None, 'zip_bad')


if __name__ == '__main__':
    unittest.main()
