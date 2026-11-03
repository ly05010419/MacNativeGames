"""NSP script: scan_nsps takes a file as a game package only when its header is one nx_extract opens (PFS0 at 0, or an XCI's HEAD at 0x100), whatever its name says: title_of falls
back to the name's [title ID], so a tagged file that is not a package would count as one. No game files or keys needed."""
import importlib.util
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


class Scan(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.said = []
        cm = mock.patch.object(nsp, 'say', self.said.append); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def test_only_packages_count(self):
        base = self.tmp / f'Game [{APP:016X}][v0].xci'; base.write_bytes(bytes(0x100) + b'HEAD' + bytes(0x100))   # a cartridge dump: no ticket, the name's tag
        upd = self.tmp / f'Update [{APP | 0x800:016X}].nsp'; pfs0(upd, [])
        page = self.tmp / f'Update [{APP | 0x800:016X}] (1).nsp'; page.write_bytes(b'<!DOCTYPE html><title>404</title>')   # a failed download saved under the game's name
        short = self.tmp / f'DLC [{APP + 0x1001:016X}].xci'; short.write_bytes(b'\0' * 0x80)   # cut off before the card header
        app, fam = nsp.scan_nsps(self.tmp)
        self.assertEqual(app, APP)
        self.assertEqual(fam['base'], base)
        self.assertEqual([p for _, p in fam['updates']], [upd])   # with the page the untagged update was one of two and left out
        self.assertEqual(fam['dlc'], [])
        self.assertEqual(sorted(m.split(':')[0] for m in self.said), sorted(f'skipping {p.name}' for p in (page, short)))

    def test_appledouble_skipped_quietly(self):
        base = self.tmp / f'Game [{APP:016X}][v0].nsp'; pfs0(base, [f'{APP:016x}0000000000000011.tik'])
        (self.tmp / ('._' + base.name)).write_bytes(b'\0\x05\x16\x07' + b'\0' * 4092)
        self.assertEqual(nsp.scan_nsps(self.tmp)[1]['base'], base)
        self.assertEqual(self.said, [])


class TitleFromPackage(unittest.TestCase):
    """A package with neither a ticket nor a [title ID] in its name (a standard-crypto dump, a repack: LNPANDY/NX2Apple#177) is asked for its title ID. The reader itself (nx_extract --title,
    the meta NCA's header under the keys' header_key) needs real keys and is not run here: its boundary is stubbed and its command line and output are checked"""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        cm = mock.patch.object(nsp, 'say', lambda *_: None); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def test_precedence_and_cost(self):
        def never(_): raise AssertionError('the package was read although a ticket or a tag names it')
        tik = self.tmp / 'a.nsp'; pfs0(tik, [f'{APP:016x}0000000000000011.tik'])
        self.assertEqual(nsp.title_of(tik, never), APP)
        tag = self.tmp / f'Game [{APP | 0x800:016X}].nsp'; pfs0(tag, ['x.nca'])
        self.assertEqual(nsp.title_of(tag, never), APP | 0x800)
        bare = self.tmp / 'Game.nsp'; pfs0(bare, ['x.nca'])
        self.assertIsNone(nsp.title_of(bare))
        self.assertEqual(nsp.title_of(bare, lambda p: APP | 0x800 if p == bare else None), APP | 0x800)

    def test_scan_uses_the_reader_for_untagged_ticketless_packages(self):
        ids = {}
        for name, tid in (('base.nsp', APP), ('upd.nsp', APP | 0x800), ('dlc.nsp', APP + 0x1001)):
            pfs0(self.tmp / name, ['x.cnmt.nca']); ids[self.tmp / name] = tid
        app, fam = nsp.scan_nsps(self.tmp, ids.get)
        self.assertEqual((app, fam['base'], [p for _, p in fam['updates']], [t for t, _, _ in fam['dlc']]), (APP, self.tmp / 'base.nsp', [self.tmp / 'upd.nsp'], [APP + 0x1001]))

    def test_scan_still_stops_when_nothing_reads(self):
        pfs0(self.tmp / 'a.nsp', ['x.nca']); pfs0(self.tmp / 'b.nsp', ['y.nca'])
        for reader in (None, lambda p: None):
            with self.assertRaisesRegex(nsp.BuildError, 'no NSP with a readable title ID'): nsp.scan_nsps(self.tmp, reader)

    def test_reader_runs_nx_extract_title_mode(self):
        keys, pkg, calls = self.tmp / 'prod.keys', self.tmp / 'a.nsp', []
        def fake_run(cmd, log, env=None, cwd=None, on_line=None, ok=()):
            calls.append((cmd, ok)); log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(self.out)
            if self.broken: raise nsp.BuildError('nx_extract exit code 1')
        with mock.patch.object(nsp, 'build_nx_extract', lambda *a: 'nx'), mock.patch.object(nsp, 'run', fake_run), nsp.package_title_reader(REPO, keys, {}) as read:
            self.out, self.broken = f'title {APP:016x}\n', False
            self.assertEqual(read(pkg), APP)
            self.assertEqual(calls[0][0], ['nx', keys, '--title', pkg]); self.assertIn(5, calls[0][1])
            self.out = '[RED] no meta NCA in a.nsp\n'
            self.assertIsNone(read(pkg))
            self.out, self.broken = '', True
            self.assertIsNone(read(pkg))

    def test_reader_without_dotnet_says_what_to_do(self):
        with mock.patch.object(nsp.shutil, 'which', lambda *a, **k: None), nsp.package_title_reader(REPO, self.tmp / 'prod.keys', {}) as read:
            with self.assertRaisesRegex(nsp.BuildError, r'needs dotnet .* add the title ID to the name'): read(self.tmp / 'Game.nsp')



def xci(path, names):   # an XCI whose root HFS0 holds a secure partition holding empty files with these names
    def hfs0(entries):   # [(name, data bytes)]
        strs = b''.join(n.encode() + b'\0' for n, _ in entries); ent, o, so, data = b'', 0, 0, b''
        for n, d in entries:   # entry: offset, size, name, hashed size, reserved, hash
            ent += struct.pack('<QQIIQ', o, len(d), so, 0, 0) + bytes(0x20); o += len(d); so += len(n) + 1; data += d
        return b'HFS0' + struct.pack('<III', len(entries), len(strs), 0) + ent + strs + data
    head = bytearray(0x200); head[0x100:0x104] = b'HEAD'; struct.pack_into('<Q', head, 0x130, 0x200)
    path.write_bytes(bytes(head) + hfs0([('update', b''), ('secure', hfs0([(n, b'') for n in names]))]))


class Merged(unittest.TestCase):
    """A merged package: base game + update + DLC in one NSP (\"[Base eShop + Update 4.0.1 - v1507328 + 1 DLC].nsp\"), or a cartridge dump with its update. Its tickets name every
    title, the first one in any order, and nx_extract opened it as the base game alone (Mario Kart 8 Deluxe 1.0.0's main is 32-bit: the build stopped). It is asked for its content
    metas and counts as each of them"""
    CONTENTS = [('Application', APP, 0), ('Patch', APP | 0x800, 1507328), ('AddOnContent', APP + 0x1001, 65536)]

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.said = []
        cm = mock.patch.object(nsp, 'say', self.said.append); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def merged_nsp(self, name='Game [Base + Update + 1 DLC].nsp', first=APP | 0x800):   # first: the title of the first ticket, the one title_of reads
        p = self.tmp / name; tiks = [f'{t:016x}{0:016x}.tik' for t in sorted({APP, APP | 0x800}, key=lambda t: t != first)]
        pfs0(p, tiks + ['a.cnmt.nca', 'b.nca', 'c.cnmt.nca', 'd.nca', 'e.cnmt.nca'])
        return p

    def test_detection(self):
        m = self.merged_nsp(); self.assertTrue(nsp.is_merged(m))
        one = self.tmp / 'one.nsp'; pfs0(one, [f'{APP:016x}0000000000000000.tik', 'a.cnmt.nca', 'b.nca']); self.assertFalse(nsp.is_merged(one))
        cart = self.tmp / 'cart.xci'; xci(cart, ['a.cnmt.nca', 'b.nca', 'c.cnmt.nca']); self.assertTrue(nsp.is_merged(cart))
        self.assertEqual(nsp.package_names(cart), ['a.cnmt.nca', 'b.nca', 'c.cnmt.nca'])
        bare = self.tmp / 'bare.xci'; bare.write_bytes(bytes(0x100) + b'HEAD' + bytes(0x100)); self.assertFalse(nsp.is_merged(bare))

    def test_one_file_is_base_update_and_dlc(self):
        m = self.merged_nsp()
        app, fam = nsp.scan_nsps(self.tmp, lambda p: None, lambda p: self.CONTENTS if p == m else None)
        self.assertEqual(app, APP)
        self.assertEqual(fam, {'base': m, 'updates': [(1507328, m)], 'dlc': [(APP + 0x1001, 65536, m)]})
        self.assertEqual(nsp.nsp_versions(fam, ['v0', 'v1507328']), {'v0': None, 'v1507328': m})
        self.assertTrue(any('holds several titles' in s for s in self.said))

    def test_merged_beside_separate_files(self):   # a newer update beside the merged package: both are listed, nsp_versions keeps each by its version
        m = self.merged_nsp(); upd = self.tmp / f'Update [{APP | 0x800:016X}][v1572864].nsp'; pfs0(upd, [f'{APP | 0x800:016x}0000000000000005.tik'])
        app, fam = nsp.scan_nsps(self.tmp, None, lambda p: self.CONTENTS)
        self.assertEqual(sorted(fam['updates']), [(1507328, m), (1572864, upd)])

    def test_without_a_reader_the_old_way(self):   # no contents reader (or an unreadable package): its first ticket names it, as before
        m = self.merged_nsp(first=APP)
        for reader in (None, lambda p: None):
            app, fam = nsp.scan_nsps(self.tmp, None, reader)
            self.assertEqual((app, fam['base'], fam['updates']), (APP, m, []))

    def test_extraction_estimate_counts_the_file_once(self):
        m = self.merged_nsp(); size = m.stat().st_size
        self.assertEqual(nsp.extract_bytes({'v1507328': (m, m)}, clones=False), size)

    def test_two_games_on_one_cartridge(self):   # Super Mario Galaxy + Super Mario Galaxy 2 on one XCI: two Application content metas, two games; --title-id names the one to pack
        GAME2 = 0x0100FD8022DAA000
        cart = self.tmp / 'Galaxy 1 + 2.xci'; xci(cart, ['a.cnmt.nca', 'b.nca', 'c.cnmt.nca', 'd.nca'])
        contents = lambda p: [('Application', APP, 0), ('Application', GAME2, 0)]
        with self.assertRaisesRegex(nsp.BuildError, r'more than one game .*--title-id'): nsp.scan_nsps(self.tmp, None, contents)
        app, fam = nsp.scan_nsps(self.tmp, None, contents, GAME2)
        self.assertEqual((app, fam), (GAME2, {'base': cart, 'updates': [], 'dlc': []}))
        with self.assertRaisesRegex(nsp.BuildError, r'no game 0100FD8022DAB000 .* which holds'): nsp.scan_nsps(self.tmp, None, contents, GAME2 + 0x1000)

    def test_extraction_names_the_application(self):   # nx_extract reads the scanned game's own content metas (--app), whichever package holds them
        m = self.merged_nsp(); calls = []
        def fake_run(cmd, log, **k): calls.append(cmd); (Path(cmd[4]) / 'ExeFS').mkdir(parents=True)
        with mock.patch.object(nsp, 'run', fake_run), mock.patch.object(nsp, 'swap_in'), mock.patch.object(nsp, 'extracted', return_value=None), \
             mock.patch.object(nsp, 'PROG', mock.MagicMock()):
            nsp.extract('nx', 'k', self.tmp / 'ex', 'v1507328', m, m, None, self.tmp / 'logs', mock.MagicMock(), APP)
        self.assertEqual(calls[0][calls[0].index('--app') + 1], f'{APP:016x}')

    def test_unreadable_contents_are_said(self):   # a merged package whose content metas cannot be read counts as its first ticket's title, in red
        self.merged_nsp(first=APP)
        app, fam = nsp.scan_nsps(self.tmp, None, lambda p: None)
        self.assertEqual(app, APP); self.assertTrue(any(s.startswith('[RED]') and 'could not be read' in s for s in self.said))

    def test_reader_runs_nx_extract_contents_mode(self):
        keys, calls = self.tmp / 'prod.keys', []
        def fake_run(cmd, log, env=None, cwd=None, on_line=None, ok=()):
            calls.append(cmd); log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(f'content Application {APP:016x} 0\ncontent Patch {APP | 0x800:016x} 1507328\ncontent AddOnContent {APP + 0x1001:016x} 65536\n')
        with mock.patch.object(nsp, 'build_nx_extract', lambda *a: 'nx'), mock.patch.object(nsp, 'run', fake_run), nsp.package_title_reader(REPO, keys, {}) as read:
            self.assertEqual(read.contents(self.tmp / 'm.nsp'), self.CONTENTS)
            self.assertEqual(calls, [['nx', keys, '--contents', self.tmp / 'm.nsp']])

    def test_keys_too_old_stop_before_the_packages_are_read(self):   # nx_extract --keys: every package once, a merged one too; its red line becomes the stop, other failures are left to later steps
        keys, m, upd, calls = self.tmp / 'prod.keys', self.tmp / 'm.nsz', self.tmp / 'u.nsp', []
        fam = {'base': m, 'updates': [(1507328, m), (1572864, upd)], 'dlc': [(APP + 0x1001, 65536, m)]}
        old = ('[RED] m.nsz (and 1 more files): prod.keys has no key_area_key_application_10 (master key 10, which came with firmware 17.0.0): the keys are older than the game; '
               'dump prod.keys again from a console on firmware 17.0.0 or later')
        def fake_run(cmd, log, env=None, cwd=None, on_line=None, ok=()):
            calls.append((cmd, ok)); log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(self.out + '\n')
            if self.broken: raise nsp.BuildError('nx_extract exit code 134')
        with mock.patch.object(nsp, 'build_nx_extract', lambda *a: 'nx'), mock.patch.object(nsp, 'run', fake_run), nsp.package_title_reader(REPO, keys, {}) as read:
            self.out, self.broken = 'the keys open all 2 packages', False
            read.check_keys(fam)
            self.assertEqual(calls[0][0], ['nx', keys, '--keys', m, upd]); self.assertIn(6, calls[0][1])
            self.out = old
            with self.assertRaisesRegex(nsp.BuildError, r'^m\.nsz \(and 1 more files\): prod\.keys has no key_area_key_application_10 .* firmware 17\.0\.0 or later$'): read.check_keys(fam)
            self.out, self.broken = 'Unhandled exception. System.IO.IOException', True
            read.check_keys(fam)
        with mock.patch.object(nsp.shutil, 'which', lambda *a, **k: None), nsp.package_title_reader(REPO, keys, {}) as read:
            read.check_keys(fam)   # no dotnet: the steps that need it say so


if __name__ == '__main__':
    unittest.main()
