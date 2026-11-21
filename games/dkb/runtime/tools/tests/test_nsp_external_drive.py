"""NSP script: a game folder on an exFAT / FAT external drive. macOS writes an AppleDouble `._<name>` beside every file there: scan_nsps must not read one as a game file, and rmtree
must treat an entry that vanished between the listing and its unlink (the `._x` that goes with `x`) as removed. No game files or keys needed."""
import errno
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)

APP = 0x010020D01AD24000


def pfs0(path, names):   # a PFS0 holding empty files with these names (enough for title_of)
    strs = b''.join(n.encode() + b'\0' for n in names); ent, o = b'', 0
    for n in names: ent += struct.pack('<QQII', 0, 0, o, 0); o += len(n) + 1
    path.write_bytes(b'PFS0' + struct.pack('<III', len(names), len(strs), 0) + ent + strs)


class ExternalDrive(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        cm = mock.patch.object(nsp, 'say', lambda m: None); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def test_scan_skips_appledouble(self):
        base = self.tmp / f'Game [{APP:016X}][v0].nsp'; pfs0(base, [f'{APP:016x}0000000000000011.tik'])
        upd = self.tmp / f'Update [{APP | 0x800:016X}].nsp'; pfs0(upd, [])
        for p in (base, upd): (self.tmp / ('._' + p.name)).write_bytes(b'\0\x05\x16\x07' + b'\0' * 4092)   # what macOS writes: 4 KB, no PFS0
        for ext in ('xci', 'nsz', 'xcz'): (self.tmp / f'._Other [{APP | 0x800:016X}].{ext}').write_bytes(b'\0' * 4096)
        app, fam = nsp.scan_nsps(self.tmp)   # with the `._` copies this raised (more than one game) or listed them as updates
        self.assertEqual(app, APP)
        self.assertEqual(fam['base'], base)
        self.assertEqual([p for _, p in fam['updates']], [upd])
        self.assertEqual(fam['dlc'], [])   # with the `._` copies the untagged update was counted more than once

    def test_rmtree_tolerates_partner_vanishing(self):
        d = self.tmp / 'tree'; (d / 'sub').mkdir(parents=True)
        for n in ('a', '._a', 'sub/b', 'sub/._b'): (d / n).write_bytes(b'x')
        real = os.unlink

        def unlink(path, *a, **kw):   # deleting `x` takes `._x` with it (and the other way round), as on an exFAT volume
            real(path, *a, **kw)
            head, name = os.path.split(path); partner = os.path.join(head, name[2:] if name.startswith('._') else '._' + name)   # a bare name under dir_fd, a full path otherwise
            try: real(partner, *a, **kw)
            except FileNotFoundError: pass
        with mock.patch('os.unlink', unlink), mock.patch('os.remove', unlink):
            nsp.rmtree(d)
        self.assertFalse(d.exists())

    def test_rmtree_reports_other_errors(self):
        d = self.tmp / 'tree'; d.mkdir(); (d / 'a').write_bytes(b'x')
        def unlink(path, *a, **kw): raise PermissionError(errno.EACCES, 'denied', str(path))
        with mock.patch('os.unlink', unlink), mock.patch('os.remove', unlink):
            with self.assertRaises(PermissionError): nsp.rmtree(d)
        self.assertTrue(d.exists())


if __name__ == '__main__':
    unittest.main()
