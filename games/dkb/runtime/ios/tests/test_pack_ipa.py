"""Repeat IPA packs match fresh packs, including clone and compressed-copy paths."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('pack_ipa', REPO / 'dm/ios/tools/pack_ipa.py')
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


class PackIpa(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory(); self.addCleanup(self.tmpdir.cleanup)
        self.tmp = Path(self.tmpdir.name)
        self.app, self.rom = self.tmp / 'app', self.tmp / 'romfs'
        (self.app / 'sub').mkdir(parents=True); (self.rom / 'déjà').mkdir(parents=True)
        (self.app / 'exe').write_bytes(b'code' * 3000); os.chmod(self.app / 'exe', 0o755)
        (self.app / 'sub/info.plist').write_bytes(b'x' * 5000)
        os.symlink('info.plist', self.app / 'sub/link')
        (self.rom / 'random.bin').write_bytes(os.urandom(30000))
        (self.rom / 'text.txt').write_bytes(b'hello world ' * 4000)
        (self.rom / 'déjà/empty').write_bytes(b'')
        (self.rom / 'gone.dat').write_bytes(b'abc' * 2000)
        self.maps = [f'Payload/DmIOS.app/romfs={self.rom}', f'Payload/DmIOS.app={self.app}']
        self.out = self.tmp / 'a.ipa'

    def run_pack(self, out=None, *opts, big=pack.BIG):
        out = out or self.out
        self.kept = self.reused = 0
        replace, clone, kept, reuse = os.replace, pack.clone, pack.Zip.kept, pack.Zip.reuse
        def cloned(old, dst):
            self.assertEqual(str(dst), str(out) + '.clone')
            self.assertFalse(Path(str(out) + '.partial').exists())
            result = clone(old, dst)
            self.assertFalse(Path(str(out) + '.partial').exists())
            return result
        def publish(src, dst):
            if str(src).endswith('.clone'):
                self.assertEqual(str(dst), str(out) + '.partial')
                self.assertFalse(Path(dst).exists())
                self.kept = Path(src).stat().st_size
                self.assertLess(self.kept, out.stat().st_size)   # the old central directory was cut before .partial appears
            return replace(src, dst)
        def kept_entry(z, e, *args):
            if e.st and stat.S_ISREG(e.st.st_mode): self.reused += 1
            return kept(z, e, *args)
        def copy_entry(z, *args):
            self.reused += 1
            return reuse(z, *args)
        with patch.object(sys, 'argv', ['pack_ipa', str(out), *opts, *self.maps]), patch.object(pack, 'BIG', big), patch.object(pack, 'CHUNK', 4096), \
                patch.object(pack.os, 'replace', publish), patch.object(pack, 'clone', cloned), patch.object(pack.Zip, 'kept', kept_entry), patch.object(pack.Zip, 'reuse', copy_entry), contextlib.redirect_stdout(io.StringIO()) as output:
            pack.main()
        self.output = output.getvalue()
        for suffix in ('.partial', '.clone', '.idx.partial'): self.assertFalse(Path(str(out) + suffix).exists())
        idx = json.loads(Path(str(out) + '.idx').read_text())
        self.assertEqual(set(idx), {'version', 'level', 'zlib', 'ipa', 'files'})
        self.assertTrue(all(len(row) == 5 for row in idx['files'].values()))
        self.assertEqual(idx['ipa'], [out.stat().st_size, out.stat().st_mtime_ns])
        return idx

    def same_as_fresh(self, *opts, big=pack.BIG):
        fresh = self.tmp / 'fresh.ipa'; self.run_pack(fresh, '--no-reuse', *opts, big=big)
        with zipfile.ZipFile(self.out) as a, zipfile.ZipFile(fresh) as b:
            self.assertIsNone(a.testzip()); self.assertEqual(a.namelist(), b.namelist())
            for ia, ib in zip(a.infolist(), b.infolist()):
                for key in ('CRC', 'file_size', 'compress_size', 'external_attr', 'date_time', 'compress_type', 'flag_bits', 'header_offset'):
                    self.assertEqual(getattr(ia, key), getattr(ib, key), (ia.filename, key))
                self.assertEqual(a.read(ia), b.read(ib), ia.filename)
        self.assertEqual(self.out.read_bytes(), fresh.read_bytes())

    def offset(self, name):
        with zipfile.ZipFile(self.out) as z: return z.getinfo('Payload/DmIOS.app/' + name).header_offset

    def test_unchanged_clone(self):
        self.run_pack()
        with zipfile.ZipFile(self.out) as z: end = z.start_dir
        with patch.object(pack, 'load', side_effect=AssertionError('unchanged file compressed')):
            self.run_pack()
        self.assertEqual(self.reused, 6); self.assertEqual(self.kept, end)
        self.same_as_fresh()

    def test_app_change_keeps_romfs_and_ignores_app_folder_time(self):
        self.run_pack()
        (self.app / 'replacement').write_bytes(b'new!' * 3000)
        os.replace(self.app / 'replacement', self.app / 'exe'); os.chmod(self.app / 'exe', 0o755)
        os.utime(self.app, (time.time() - 7200,) * 2)
        self.run_pack()
        self.assertEqual(self.reused, 5); self.assertEqual(self.kept, self.offset('exe'))
        self.assertGreater(self.kept, self.offset('romfs/text.txt')); self.same_as_fresh()

    def test_change_in_first_mapping(self):
        self.run_pack(); (self.rom / 'déjà/empty').write_bytes(b'changed')
        self.run_pack(); self.assertEqual(self.reused, 5)
        self.assertEqual(self.kept, self.offset('romfs/déjà/empty')); self.same_as_fresh()

    def test_changed_added_removed_and_same_size_rewrite(self):
        self.run_pack()
        (self.rom / 'text.txt').write_bytes(b'changed' * 4000); (self.rom / 'gone.dat').unlink(); (self.rom / 'new.bin').write_bytes(b'n' * 9000)
        source = self.app / 'sub/info.plist'; before = source.stat()
        source.write_bytes(b'y' * before.st_size); os.utime(source, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assertNotEqual(source.stat().st_ctime_ns, before.st_ctime_ns)
        self.run_pack(); self.assertEqual(self.reused, 3); self.same_as_fresh()

    def test_caller_order_and_parents_once_before_children(self):
        msl, aoc, pso = [self.tmp / n for n in ('msl', 'aoc', 'pso')]
        for folder in (msl / 'lib', aoc, pso): folder.mkdir(parents=True)
        for file in (msl / 'lib/a.metallib', aoc / 'content', pso / 'archive'): file.write_bytes(b'fixture')
        self.maps = [self.maps[0], f'Payload/DmIOS.app/aoc={aoc}', f'Payload/DmIOS.app/pso={pso}', f'Payload/DmIOS.app/msl/lib={msl / "lib"}', self.maps[1]]
        self.run_pack()
        with zipfile.ZipFile(self.out) as z:
            names = z.namelist(); self.assertEqual(len(names), len(set(names)))
            for i, name in enumerate(names):
                parts = name.rstrip('/').split('/')
                for end in range(1, len(parts)): self.assertIn('/'.join(parts[:end]) + '/', names[:i])
            for name in ('Payload/', 'Payload/DmIOS.app/', 'Payload/DmIOS.app/msl/'):
                self.assertEqual(z.getinfo(name).date_time, (1980, 1, 1, 0, 0, 0))
                self.assertEqual(z.getinfo(name).external_attr >> 16, pack.DIR_MODE)
            positions = [names.index('Payload/DmIOS.app/' + n) for n in ('romfs/text.txt', 'aoc/content', 'pso/archive', 'msl/lib/a.metallib', 'exe')]
            self.assertEqual(positions, sorted(positions))
        self.same_as_fresh()
        self.maps.reverse(); self.run_pack()
        self.assertLess(self.offset('exe'), self.offset('romfs/text.txt')); self.same_as_fresh()
        self.maps.append(self.maps[0])
        with self.assertRaisesRegex(SystemExit, 'twice'): self.run_pack()

    def test_nested_mapping_before_parent(self):
        extra = self.tmp / 'extra'; extra.mkdir(); (extra / 'child').write_bytes(b'child')
        self.maps.insert(0, f'Payload/DmIOS.app/romfs/deep={extra}')
        self.run_pack(); self.run_pack(); self.assertEqual(self.reused, 7); self.same_as_fresh()

    def test_clone_failure_copies_compressed(self):
        self.run_pack()
        with patch.object(pack, 'clone', return_value=False), patch.object(pack, 'load', side_effect=AssertionError('recompressed')):
            self.run_pack()
        self.assertEqual(self.kept, 0); self.assertEqual(self.reused, 6); self.same_as_fresh()

    def test_symlink_and_folder_headers(self):
        self.run_pack(); parent = (self.app / 'sub').stat()
        (self.app / 'sub/link').unlink(); os.symlink('../exe', self.app / 'sub/link')
        # Isolate the symlink header change even when relinking crosses a ZIP timestamp tick.
        os.utime(self.app / 'sub', ns=(parent.st_atime_ns, parent.st_mtime_ns))
        self.run_pack(); self.assertEqual(self.kept, self.offset('sub/link')); self.same_as_fresh()
        os.utime(self.app / 'sub', (time.time() - 7200,) * 2)
        self.run_pack(); self.assertEqual(self.kept, self.offset('sub/')); self.same_as_fresh()
        os.chmod(self.app / 'sub', 0o700)
        self.run_pack(); self.assertGreater(self.kept, self.offset('sub/link')); self.same_as_fresh()

    def test_store_flag_change(self):
        self.run_pack(); self.run_pack(None, '--store', '.TXT'); self.assertEqual(self.reused, 5)
        self.same_as_fresh('--store', '.txt')

    def test_stale_replaced_and_malformed_index(self):
        self.run_pack(); index = Path(str(self.out) + '.idx')
        original = index.read_text()
        other = self.tmp / 'other.ipa'; self.run_pack(other)
        os.replace(other, self.out); index.write_text(original)
        self.run_pack(); self.assertEqual(self.reused, 0); self.assertEqual(self.kept, 0)
        for bad in ('{not json', '[]', '{}'):
            index.write_text(bad); self.run_pack(); self.assertEqual(self.reused, 0); self.same_as_fresh()
        index.unlink(); self.run_pack(); self.assertEqual(self.reused, 0)
        idx = json.loads(index.read_text()); idx['files']['Payload/DmIOS.app/exe'] = ['bad']
        index.write_text(json.dumps(idx)); self.run_pack(); self.assertEqual(self.reused, 5); self.same_as_fresh()

    def test_zlib_version_mismatch_forces_full_pack(self):
        idx = self.run_pack(); idx['zlib'] = 'different'
        Path(str(self.out) + '.idx').write_text(json.dumps(idx))
        self.run_pack(); self.assertEqual(self.kept, 0); self.assertEqual(self.reused, 0); self.same_as_fresh()

    def test_streamed_reuse_and_fallback_to_stored(self):
        self.run_pack(big=1024); self.run_pack(big=1024); self.assertEqual(self.reused, 6)
        (self.rom / 'random.bin').write_bytes(os.urandom(30000))
        self.run_pack(big=1024); self.assertEqual(self.reused, 5); self.same_as_fresh(big=1024)
        self.run_pack(); self.assertEqual(self.reused, 6); self.same_as_fresh()

    def test_stream_drains_pool_before_later_jobs(self):
        load, stream, events, submitted = pack.load, pack.Zip.stream, [], []
        class Pool(pack.cf.ThreadPoolExecutor):
            def submit(self, fn, path, deflate):
                submitted.append(Path(path).name)
                return super().submit(fn, path, deflate)
        def loaded(path, deflate):
            result = load(path, deflate); events.append(Path(path).name); return result
        def streamed(z, e, deflate):
            self.assertIn('random.bin', events); self.assertNotIn('exe', submitted)
            return stream(z, e, deflate)
        with patch.object(pack, 'load', loaded), patch.object(pack.Zip, 'stream', streamed), patch.object(pack.cf, 'ThreadPoolExecutor', Pool): self.run_pack(big=40000)
        self.same_as_fresh(big=40000)

    def test_local_header_mismatch_recompresses(self):
        self.run_pack(); offset = self.offset('exe')
        with open(self.out, 'r+b') as f: f.seek(offset + 10); f.write(b'\x00\x00')
        idxp = Path(str(self.out) + '.idx'); idx = json.loads(idxp.read_text()); idx['ipa'] = [self.out.stat().st_size, self.out.stat().st_mtime_ns]; idxp.write_text(json.dumps(idx))
        self.run_pack(); self.assertEqual(self.reused, 5); self.same_as_fresh()

    def test_zip64_local_headers(self):
        header = pack.header
        def zip64(e, method, crc, csize, usize, z64=False): return header(e, method, crc, csize, usize, True)
        with patch.object(pack, 'header', zip64):
            self.run_pack(); self.run_pack(); self.assertEqual(self.reused, 6)
            self.assertGreater(self.kept, self.offset('sub/link')); self.same_as_fresh()
        self.run_pack(); self.assertEqual(self.reused, 0); self.assertEqual(self.kept, 0); self.same_as_fresh()

    def test_clone_uses_checked_inode_after_path_replaced(self):
        self.run_pack(); clone = pack.clone; old_bytes = self.out.read_bytes()
        def checked_clone(old, dst):
            self.assertEqual(os.pread(old.fileno(), len(old_bytes), 0), old_bytes)
            replacement = self.tmp / 'replacement'; replacement.write_bytes(b'wrong inode')
            with open(replacement, 'r+b') as f: f.truncate(len(old_bytes))
            os.replace(replacement, self.out)
            return clone(old, dst)
        with patch.object(pack, 'clone', checked_clone): self.run_pack()
        self.assertGreater(self.kept, 0); self.same_as_fresh()

    def test_stat_before_publish_and_stale_partial_cleanup(self):
        self.run_pack()
        target = self.tmp / 'protected'; target.write_bytes(b'unchanged')
        Path(str(self.out) + '.idx.partial').symlink_to(target)
        replace, expected = os.replace, []
        def publish(src, dst):
            if str(src).endswith('.partial') and str(dst) == str(self.out):
                st = os.stat(src); expected.append([st.st_size, st.st_mtime_ns])
                replace(src, dst); os.utime(dst, ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000))
            else: replace(src, dst)
        with patch.object(pack.os, 'replace', publish), patch.object(sys, 'argv', ['pack_ipa', str(self.out), *self.maps]), contextlib.redirect_stdout(io.StringIO()): pack.main()
        self.assertFalse(Path(str(self.out) + '.idx.partial').exists())
        self.assertEqual(json.loads(Path(str(self.out) + '.idx').read_text())['ipa'], expected[0])
        self.assertEqual(target.read_bytes(), b'unchanged')

    def test_failure_preserves_old_ipa_and_index(self):
        self.run_pack(); before, idx = self.out.read_bytes(), Path(str(self.out) + '.idx').read_bytes()
        (self.app / 'exe').write_bytes(b'changed')
        with patch.object(pack, 'load', side_effect=OSError('read failed')):
            with self.assertRaisesRegex(OSError, 'read failed'): self.run_pack()
        self.assertEqual(self.out.read_bytes(), before); self.assertEqual(Path(str(self.out) + '.idx').read_bytes(), idx)
        self.assertFalse(Path(str(self.out) + '.partial').exists()); self.assertFalse(Path(str(self.out) + '.clone').exists())

    def test_pool_size_change_preserves_old_ipa_and_index(self):
        self.run_pack(); before, index = self.out.read_bytes(), Path(str(self.out) + '.idx').read_bytes()
        source, load = self.app / 'exe', pack.load
        for store in (False, True):
            for grow in (False, True):
                with self.subTest(store=store, grow=grow):
                    source.write_bytes(b'code' * 3000)
                    def changed(path, deflate):
                        if path == str(source):
                            data = source.read_bytes(); source.write_bytes(data + b'grow' if grow else data[:-4])
                        return load(path, deflate)
                    with patch.object(pack, 'load', changed), self.assertRaises(SystemExit) as error:
                        self.run_pack(None, *(['--store', 'exe'] if store else []))
                    self.assertEqual(str(error.exception), f'[RED] pack_ipa: {source} changed size while it was packed')
                    self.assertEqual(self.out.read_bytes(), before); self.assertEqual(Path(str(self.out) + '.idx').read_bytes(), index)
                    for suffix in ('.partial', '.clone'): self.assertFalse(Path(str(self.out) + suffix).exists())

    def test_stream_size_change_preserves_old_ipa_and_index(self):
        self.run_pack(); before, index = self.out.read_bytes(), Path(str(self.out) + '.idx').read_bytes()
        source, stream = self.app / 'exe', pack.Zip.stream
        for store in (False, True):
            for grow in (False, True):
                with self.subTest(store=store, grow=grow):
                    source.write_bytes(b'code' * 3000)
                    def changed(z, e, deflate):
                        if e.path == str(source):
                            data = source.read_bytes(); source.write_bytes(data + b'grow' if grow else data[:-4])
                        return stream(z, e, deflate)
                    with patch.object(pack.Zip, 'stream', changed), self.assertRaisesRegex(SystemExit, 'changed size while it was packed'):
                        self.run_pack(None, *(['--store', 'exe'] if store else []), big=1024)
                    self.assertEqual(self.out.read_bytes(), before); self.assertEqual(Path(str(self.out) + '.idx').read_bytes(), index)
                    for suffix in ('.partial', '.clone'): self.assertFalse(Path(str(self.out) + suffix).exists())

    def test_non_apfs_mapping_never_reuses_files(self):
        self.assertEqual(pack.fs_type(self.tmp / 'missing'), b'')
        self.run_pack()
        for unsafe, filesystem, expected in (({str(self.rom)}, b'exfat', 2), ({str(self.app)}, b'exfat', 4), ({str(self.app), str(self.rom)}, b'', 0)):
            with self.subTest(unsafe=unsafe, filesystem=filesystem):
                with patch.object(pack, 'fs_type', side_effect=lambda root: filesystem if root in unsafe else b'apfs') as fs, \
                        patch.object(pack, 'load', wraps=pack.load) as loaded:
                    self.run_pack()
                self.assertEqual(self.reused, expected)
                self.assertEqual({call.args[0] for call in fs.call_args_list}, {str(self.app), str(self.rom)})
                self.assertEqual(fs.call_count, 2)
                self.assertEqual(len(loaded.call_args_list), 6 - expected)
                self.assertTrue(all(any(Path(call.args[0]).is_relative_to(root) for root in unsafe) for call in loaded.call_args_list))
                for arc, root in (mapping.split('=', 1) for mapping in self.maps):
                    message = f'reuse off for {arc}: not APFS'
                    self.assertEqual(self.output.count(message), int(root in unsafe))
                    if root in unsafe: self.assertIn(message, self.output.splitlines()[-1])
                if expected == 4: self.assertGreater(self.kept, self.offset('romfs/text.txt'))
                self.same_as_fresh()

    def test_progress_excludes_kept_bytes(self):
        with patch.object(nsp, 'PROG'):
            self.out.write_bytes(b'0' * 1000)
            watch = nsp.IpaWatch(REPO, self.tmp, self.out)
            watch.line('pack IPA'); watch.line('pack: 8 entries, 2000 bytes')
            watch.part.write_bytes(b'0' * 1000); self.assertIsNone(watch.poll())
            watch.line('pack: keeping 800 bytes')
            watch.part.write_bytes(b'0' * 850)
            self.assertEqual(watch.poll(), {'done': 50, 'total': 200, 'est': True})


if __name__ == '__main__': unittest.main()
