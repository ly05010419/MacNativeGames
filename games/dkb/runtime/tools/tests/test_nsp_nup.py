"""NSP script with a NUP container (scripts/pack_nup.py): the container is read and checked, its versions and DLC packed into what the script's later steps read, its control data saved,
and the script asks for no keys. The NUPs are written here (SPEC v1, https://github.com/a55uka/nup). The run stops at the prerequisite check; no game files, keys or Xcode needed."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import nacp
import romfs_image
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
pack_nup = nsp.pack_nup
zstd = pack_nup.zstd

TID = 0x040074A01BF12000   # a game with a profile in dm/games, whose registered version is an update
VER = 458752


def npdm(tid):
    d = bytearray(0x100)
    d[:4] = b'META'
    struct.pack_into('<II', d, 0x70, 0x80, 0x20)
    d[0x80:0x84] = b'ACI0'
    struct.pack_into('<Q', d, 0x90, tid)
    return bytes(d)


def tlv(recs):
    return b''.join(struct.pack('<HHI', t, 0, len(v)) + v for t, v in recs)


def write_nup(path, packages):
    """packages: [(kind, title ID, version, {'exefs': {name: bytes}, 'romfs': {path: bytes}}, meta bytes)] → a NUP file, every file of 200 bytes or more zstd-compressed"""
    dirs, files, slots, pool = [], [], [], bytearray()
    def walk(name, tree):   # tree: {name: bytes or dict}; dirs pre-order, children contiguous and sorted
        d = len(dirs); dirs.append([name, 0, 0])
        kids = sorted(tree.items(), key=lambda kv: kv[0].encode())
        first = len(slots); slots.extend([None] * len(kids)); dirs[d][1:] = [first, len(kids)]
        for j, (n, v) in enumerate(kids):
            if isinstance(v, bytes): slots[first + j] = ('f', len(files)); files.append((n, v))
        for j, (n, v) in enumerate(kids):
            if isinstance(v, dict): slots[first + j] = ('d', walk(n, v))
        return d
    def nest(flat):
        t = {}
        for p, v in flat.items():
            *ds, f = p.split('/'); c = t
            for x in ds: c = c.setdefault(x, {})
            c[f] = v
        return t
    recs = []
    for kind, tid, ver, tree, meta in packages:
        t = {k: v if k == 'exefs' else nest(v) for k, v in tree.items()}
        root = walk('', t)
        sub = {n: s[1] for n, s in zip(sorted(t), slots[dirs[root][1]:dirs[root][1] + dirs[root][2]])}
        recs.append([tid, root, sub.get('exefs', 0xFFFFFFFF), sub.get('romfs', 0xFFFFFFFF), ver, kind, meta])
    def name(s):
        b = s.encode(); off = len(pool); pool.extend(b); return off, len(b)
    toc_len = 0x30 + len(recs) * 0x40 + len(dirs) * 0x18 + len(files) * 0x50 + len(slots) * 4
    pool_off = toc_len
    names_d = [name(d[0]) for d in dirs]; names_f = [name(f[0]) for f in files]
    toc_size = (pool_off + len(pool) + 15) // 16 * 16
    pos = 0x80 + toc_size
    metas = []
    for r in recs:
        metas.append((pos, r[6])); pos = (pos + len(r[6]) + 15) // 16 * 16
    blob, blobs = pos, []
    ext = []
    for n, data in files:
        stored, codec = (zstd.compress(data), 1) if len(data) >= 200 else (data, 0)
        if not data: ext.append((0, 0, 0)); continue
        ext.append((pos, len(stored), codec)); blobs.append((pos, stored)); pos = (pos + len(stored) + 15) // 16 * 16
    toc = bytearray(struct.pack('<4sIQQQQQ', b'TOC\x01', len(recs), len(dirs), len(files), pool_off, len(pool), 0))
    for r, (moff, m) in zip(recs, metas):
        toc += struct.pack('<QIIIIBBHIQI20x', r[0], r[1], r[2], r[3], r[4], r[5], 0, 0, 0, moff if m else 0, len(m))
    for (o, ln), d in zip(names_d, dirs): toc += struct.pack('<IIIIQ', o, ln, d[1], d[2], 0)
    for (o, ln), (n, data), (eo, es, c) in zip(names_f, files, ext): toc += struct.pack('<IIQQQ32sBBH12x', o, ln, len(data), eo, es, hashlib.sha256(data).digest(), c, 0, 0)
    for k, i in slots: toc += struct.pack('<I', i if k == 'd' else len(dirs) + i)
    toc += pool; toc += b'\0' * (toc_size - len(toc))
    body = bytearray(pos)
    body[0x80:0x80 + toc_size] = toc
    for moff, m in metas: body[moff:moff + len(m)] = m
    for o, b in blobs: body[o:o + len(b)] = b
    uuid = hashlib.sha256(b''.join(struct.pack('<QIB', r[0], r[4], r[5]) for r in recs)).digest()[:16]
    body[:0x80] = struct.pack('<4sHHQQ32sQQII16s32s', b'NUP\x01', 0x80, 1, 0x80, toc_size, hashlib.sha256(toc).digest(), blob, pos, len(recs), 0, uuid, b'\0' * 32)
    Path(path).write_bytes(body)


def version(main=b'code', marker='marker', tid=TID):
    return {'exefs': {'main': main, 'main.npdm': npdm(tid), '.listing': b'not a module'}, 'romfs': {f'dir/{marker}': b'data' * 100, 'empty': b''}}


def control_nacp(name, jp, display):   # a console NACP's title entries (0x300 per language, Japanese the third) and display version
    b = bytearray(0x4000)
    b[:len(name)] = name.encode(); b[2 * 0x300:2 * 0x300 + len(jp.encode())] = jp.encode(); b[0x3060:0x3060 + len(display)] = display.encode()
    return bytes(b)


T_NAME = 5   # the packer's name label (SPEC 0x0005), which is not the game's name
META = tlv([(T_NAME, b'TG'), (pack_nup.T_NACP, control_nacp('Test Game', 'テス', '1.2.3')), (pack_nup.T_ICON, b'\xff\xd8 icon')])


@unittest.skipIf(zstd is None, 'no zstd module')
class Base(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.dir = Path(td.name).resolve() / 'game'; self.dir.mkdir()
        self.path = self.dir / 'game.nup'
        write_nup(self.path, [(0, TID, 0, version(marker='old'), META), (1, TID, VER, version(main=b'new code', marker='new'), b''),
                              (2, TID + 0x1001, 0, {'romfs': {'dlcfile': b'dlc'}}, b'')])

    def open(self):
        n = pack_nup.Nup(self.path); self.addCleanup(n.close); return n


class ExeFSNames(unittest.TestCase):
    def test_rejects_names_that_are_not_path_components(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            target = root / 'sentinel'
            target.write_bytes(b'original')
            archive = root / 'game.nup'
            dst = root / 'build' / 'ExeFS'
            for name in (str(target), '../sentinel', 'nested/file', '', '.', '..', 'bad\0name'):
                with self.subTest(name=name):
                    tree = {'exefs': {name: b'overwritten'}}
                    write_nup(archive, [(0, TID, 0, tree, b'')])
                    try:
                        with pack_nup.Nup(archive) as n:
                            with self.assertRaisesRegex(pack_nup.Error, 'invalid ExeFS name'):
                                pack_nup.exefs(n, n.packages[0], dst)
                    finally:
                        self.assertEqual(target.read_bytes(), b'original')
                    self.assertFalse(dst.with_name('ExeFS.partial').exists())


class Read(Base):
    def test_scan(self):
        n = self.open()
        app, base, vers, dlc = pack_nup.scan(n)
        self.assertEqual((app, base.kind, sorted(vers), sorted(dlc)), (TID, 0, [VER], [TID + 0x1001]))

    def test_damaged_file_refused(self):
        b = bytearray(self.path.read_bytes()); b[0x90] ^= 1; self.path.write_bytes(b)   # a TOC byte
        with self.assertRaisesRegex(pack_nup.Error, 'SHA-256'): pack_nup.Nup(self.path)
        self.path.write_bytes(b[:-16])
        with self.assertRaisesRegex(pack_nup.Error, 'header says'): pack_nup.Nup(self.path)

    def test_damaged_content_refused(self):
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        i = next(i for i, f in enumerate(n.files) if f[5] == 1)
        b = bytearray(self.path.read_bytes()); b[n.files[i][3] + n.files[i][4] - 1] ^= 0xFF; n.close(); self.path.write_bytes(b)
        n = self.open()
        with self.assertRaises(pack_nup.Error): n.read(i)

    def test_no_base_refused(self):
        write_nup(self.path, [(1, TID, VER, version(), b'')])
        with self.assertRaisesRegex(pack_nup.Error, 'no base game'): pack_nup.scan(self.open())

    def test_folder_or_file(self):
        self.assertEqual(nsp.nup_file(self.dir), self.path)
        self.assertEqual(nsp.nup_file(self.path), self.path)
        (self.dir / 'game.nsp').write_bytes(b'x')
        self.assertIsNone(nsp.nup_file(self.dir))   # the packages win, as over a dump's layout


class Pack(Base):
    def out(self): return self.dir / 'out'

    def test_empty_base_with_complete_update(self):
        for tree in ({'exefs': {}}, {}):
            write_nup(self.path, [(0, 0, 0, tree, b''), (1, TID, VER, version(), META)])
            with pack_nup.Nup(self.path) as n:
                app, base, vers, _ = pack_nup.scan(n)
                self.assertEqual(app, TID)
                pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
                self.assertIn('dir/marker', romfs_image.read_index(self.out() / 'RomFS.img'))
                self.assertEqual((self.out() / 'ExeFS/main').read_bytes(), b'code')
                with self.assertRaisesRegex(pack_nup.Error, 'no exefs/main'):
                    pack_nup.pack_version(n, base, base, app, self.out() / 'base')

    def test_selected_version_needs_romfs(self):
        tree = {'exefs': version()['exefs']}
        write_nup(self.path, [(0, TID, 0, tree, b''), (1, TID, VER, tree, b'')])
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        with self.assertRaisesRegex(pack_nup.Error, 'no romfs/'):
            pack_nup.pack_version(n, vers[VER], base, app, self.out())
        self.assertFalse(self.out().exists())

    def test_update_without_romfs_uses_base(self):
        write_nup(self.path, [(0, TID, 0, version(marker='base'), b''),
                              (1, TID, VER, {'exefs': version(main=b'updated')['exefs']}, b'')])
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
        self.assertIn('dir/base', romfs_image.read_index(self.out() / 'RomFS.img'))
        self.assertEqual((self.out() / 'ExeFS/main').read_bytes(), b'updated')

    def test_mismatched_program_ids_refused(self):
        write_nup(self.path, [(0, TID, 0, version(), b''), (1, TID, VER, version(tid=TID + 0x2000), b'')])
        with self.assertRaisesRegex(pack_nup.Error, 'different program IDs'): pack_nup.scan(self.open())

    def test_update_packed_on_its_own_with_control_data(self):
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out() / 'u', say=lambda s: None)
        u = self.out() / 'u'
        self.assertEqual((u / 'ExeFS/main').read_bytes(), b'new code')
        self.assertFalse((u / 'ExeFS/.listing').exists())
        names = romfs_image.read_index(u / 'RomFS.img')
        self.assertIn('dir/new', names); self.assertNotIn('dir/old', names); self.assertEqual(names['empty'], (False, names['empty'][1], 0))
        self.assertFalse((u / 'control.nacp').exists())   # the update carries no control data: the script takes the base's
        pack_nup.pack_version(n, base, base, app, self.out() / 'b', say=lambda s: None)
        b = self.out() / 'b'
        self.assertEqual((nacp.name(b / 'control.nacp'), nacp.version(b / 'control.nacp')), ('Test Game', '1.2.3'))
        self.assertEqual((b / 'icon.jpg').read_bytes(), b'\xff\xd8 icon')
        jp = (b / 'control.nacp').read_bytes()[2 * 0x300:2 * 0x300 + 0x200].split(b'\0')[0]
        self.assertEqual(jp.decode(), 'テス')

    def test_name_label_without_nacp_writes_none(self):   # the script then takes the profile's name, as for a dump
        write_nup(self.path, [(0, TID, 0, version(), tlv([(T_NAME, b'TG'), (pack_nup.T_ICON, b'\xff\xd8 icon')]))])
        n = self.open(); app, base, _, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, base, base, app, self.out(), say=lambda s: None)
        self.assertFalse((self.out() / 'control.nacp').exists())
        self.assertEqual((self.out() / 'icon.jpg').read_bytes(), b'\xff\xd8 icon')

    def test_repeat_keeps_what_is_unchanged(self):
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
        main, img = self.out() / 'ExeFS/main', self.out() / 'RomFS.img'
        before = main.stat().st_ino, img.stat().st_mtime_ns
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
        self.assertEqual((main.stat().st_ino, img.stat().st_mtime_ns), before)

    def test_dlc(self):
        n = self.open(); app, _, _, dlc = pack_nup.scan(n)
        aoc = self.out() / 'aoc'; aoc.mkdir(parents=True); (aoc / '7.img').write_bytes(b'gone')
        pack_nup.pack_dlc(n, app, dlc, aoc, say=lambda s: None)
        self.assertEqual(sorted(p.name for p in aoc.iterdir()), ['1.img', '1.img.inputs.json'])
        self.assertIn('dlcfile', romfs_image.read_index(aoc / '1.img'))

    def test_nsp_dlc_not_reused_after_nup(self):   # the NUP packs into the aoc/ the NSP extraction keyed: the same NSPs again extract again
        n = self.open(); app, _, _, dlc = pack_nup.scan(n)
        work, logs, p = self.out(), self.out() / 'logs', self.dir / 'dlc.nsp'
        logs.mkdir(parents=True); p.write_bytes(b'nsp')
        runs = []
        def extract(argv, log, **kw): runs.append(argv); Path(argv[4]).parent.mkdir(parents=True, exist_ok=True); Path(argv[4]).write_bytes(b'from the NSP')
        nsp_fam, nup_fam = {'dlc': [(TID + 0x1001, 0, p)]}, {'dlc': [(t, None, k) for t, k in dlc.items()], 'nup': n}
        with mock.patch.object(nsp, 'run', side_effect=extract), mock.patch.object(nsp, 'is_merged', return_value=False):
            for fam in (nsp_fam, nsp_fam, nup_fam): aoc = nsp.extract_aoc('nx', 'keys', REPO, work, app, fam, logs)
            self.assertEqual(len(runs), 1)   # reused while the NSPs and aoc/ are unchanged
            self.assertIn('dlcfile', romfs_image.read_index(aoc / '1.img'))
            nsp.extract_aoc('nx', 'keys', REPO, work, app, nsp_fam, logs)
        self.assertEqual(len(runs), 2)
        self.assertEqual(sorted(f.name for f in aoc.iterdir()), ['1.img'])
        self.assertEqual((aoc / '1.img').read_bytes(), b'from the NSP')


class Stop(Exception): pass


class Main(Base):
    def run_main(self, target):
        seen, said = {}, []
        def stop(env, *a): seen.update(env); raise Stop()
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'configure_dotnet'), mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROD_KEYS', ''), \
             mock.patch.object(nsp, 'check_prereqs', side_effect=stop), mock.patch.object(nsp, 'say', side_effect=said.append), \
             mock.patch.object(nsp, 'ask', side_effect=AssertionError('asked for something')), mock.patch.object(nsp, 'package_title_reader', side_effect=AssertionError('read a package')):
            with self.assertRaises(Stop): nsp.main(nsp.parse_args([str(target)]))
        return Path(seen['DM_ROOT']), said

    def test_needs_no_keys_and_builds_beside_the_nup(self):
        for target in (self.dir, self.path):
            work, said = self.run_main(target)
            self.assertEqual(work, self.dir / f'{TID:016X}_build')
            self.assertTrue(any(s.startswith('a NUP: game.nup') for s in said), said)


if __name__ == '__main__':
    unittest.main()
