"""NSP script Mac app (scripts/build_ipa_from_nsp.py --platform mac-app, dm/mac/pack_app.py, dm/mac/launcher.c): the bundle's layout, Info.plist and ad-hoc signature without the hardened
runtime; the launcher's environment for the host (the bundle's files, the save, temporary files and log in the player's Library), the previous log kept, environment files, the host's exit
code returned and SIGTERM forwarded to it, an alert when the game data is missing; the script's options and its pack_app.py command line.
No game files, keys or Xcode needed: a stand-in host compiled here records what it was given (temporary files only in a temporary folder)."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import types
import unittest
import uuid
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / 'scripts/build_ipa_from_nsp.py'
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', SCRIPT)
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
spec = importlib.util.spec_from_file_location('pack_app', REPO / 'dm/mac/pack_app.py')
pack_app = importlib.util.module_from_spec(spec); spec.loader.exec_module(pack_app)
TID = '01008CF01BAAC000'
TEST_ROOT = Path(os.environ.get('NXNATIVE_TEST_ROOT', tempfile.gettempdir()))

HOST_C = r'''#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
extern char **environ;
static void term(int s) { (void)s; _exit(3); }
int main(int argc, char **argv) {
    char p[1024]; snprintf(p, sizeof p, "%s/host_run.txt", getenv("HOME")); FILE *f = fopen(p, "w");
    for (char **e = environ; *e; e++) fprintf(f, "%s\n", *e);
    for (int i = 0; i < argc; i++) fprintf(f, "argv%d=%s\n", i, argv[i]);
    char cwd[1024]; fprintf(f, "cwd=%s\n", getcwd(cwd, sizeof cwd)); fclose(f);
    printf("standin host output\n"); fflush(stdout);
    if (getenv("STANDIN_WAIT")) { signal(SIGTERM, term); for (;;) pause(); }
    return 7;
}
'''


class Copy(unittest.TestCase):
    def test_argument_budget(self):
        # A small exec limit exercises long, multibyte paths and a large inherited environment without thousands of files.
        srcs = [Path('/shader build') / ('\u00e9' * 80) / f'{i}_b.metallib' for i in range(100)]
        dst = Path('/Game.app/Contents/Resources/msl/b-0123456789ab')
        env = {'PACK_APP_TEST': 'x' * 4096}
        for clone in (False, True):
            with self.subTest(clone=clone), mock.patch.dict(os.environ, env, clear=True), mock.patch.object(os, 'sysconf', return_value=8192), mock.patch.object(pack_app, 'run') as run:
                pack_app.copy(iter(srcs), dst, clone)
                self.assertGreater(run.call_count, 1)
                copied = []
                flags = ['/bin/cp', '-R'] + (['-c'] if clone else [])
                for call in run.call_args_list:
                    cmd = call.args[0]
                    self.assertEqual(cmd[:len(flags)], flags); self.assertEqual(cmd[-1], dst)
                    copied.extend(cmd[len(flags):-1])
                    size = sum(len(os.fsencode(p)) + 1 for p in cmd) + sum(len(os.fsencode(f'{k}={v}')) + 1 for k, v in env.items())
                    self.assertLess(size, 8192)
                self.assertEqual(copied, srcs)

    def test_no_room_for_source(self):
        with mock.patch.object(os, 'sysconf', return_value=4096), mock.patch.dict(os.environ, {'PACK_APP_TEST': 'x' * 4096}, clear=True), mock.patch.object(pack_app, 'run') as run:
            with self.assertRaisesRegex(SystemExit, 'copy arguments exceed ARG_MAX'):
                pack_app.copy([Path('/source.metallib')], Path('/out'), False)
            run.assert_not_called()

    @unittest.skipUnless(sys.platform == 'darwin', 'exercises the macOS exec limit')
    def test_long_paths(self):
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as tmp:
            src = Path(tmp) / ('a' * 200) / ('b' * 200) / ('c' * 200); src.mkdir(parents=True)
            dst = Path(tmp) / 'out'; dst.mkdir()
            files = [src / f'{i:04d}_b.metallib' for i in range(2000)]
            for p in files: p.write_bytes(p.name.encode())
            self.assertGreater(sum(len(os.fsencode(p)) + 1 for p in files), os.sysconf('SC_ARG_MAX'))
            pack_app.copy(files, dst, False)
            self.assertEqual(sorted(p.name for p in dst.iterdir()), [p.name for p in files])
            for p in files: self.assertEqual((dst / p.name).read_bytes(), p.name.encode())


@unittest.skipUnless(sys.platform == 'darwin', 'uses macOS atomic bundle replacement')
class CloneProbe(unittest.TestCase):
    """can_clone with the real clonefile: a RomFS image is one file (os.walk of a file yields nothing), a folder is probed by its first file."""
    def test_image_and_folder(self):
        TEST_ROOT.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as t:
            t = Path(t)
            for d in ('romfs/sub', 'empty', 'app'): (t / d).mkdir(parents=True)
            (t / 'romfs.img').write_bytes(b'DMROMFS1'); (t / 'romfs/sub/file').write_bytes(b'file')
            if not pack_app.can_clone(t / 'romfs', t / 'app'): self.skipTest(f'no APFS clones in {TEST_ROOT}')
            self.assertTrue(pack_app.can_clone(t / 'romfs.img', t / 'app'))
            self.assertFalse(pack_app.can_clone(t / 'empty', t / 'app'))
            self.assertEqual(sorted(p.name for p in (t / 'app').iterdir()), [])   # no probe left behind


class PackReuse(unittest.TestCase):
    """Call the real pack entry point; only external compiler, icon and signing tools are faked. No host is run."""
    def setUp(self):
        TEST_ROOT.mkdir(parents=True, exist_ok=True)
        self.tmp = Path(tempfile.mkdtemp(prefix='pack-reuse-', dir=TEST_ROOT)).resolve()
        self.addCleanup(shutil.rmtree, self.tmp)
        t = self.tmp
        for d in ('patched', 'romfs/empty', 'msl', 'profile', 'font', 'aoc/1', 'pso', 'bin', 'sdk', 'source', 'host', 'tools', 'ios/Sources'):
            (t / d).mkdir(parents=True)
        files = {
            'dmhost': b'host', 'patched/main.patched.bin': b'code', 'patched/main.fixups.tsv': b'fixups',
            'patched/main.gates.tsv': b'gates', 'patched/main.cuts.tsv': b'cuts', 'patched/main.obs.tsv': b'obs', 'patched/nro.tsv': b'nro',
            'romfs/marker': b'game', 'msl/k.metallib': b'lib', 'msl/k.json': b'{}', 'msl/inputs.tsv': b'stamp',
            'font/nintendo_ext.ttf': b'font', 'aoc/1/data': b'dlc', 'pso/gpu.mtl4a': b'archive',
            'pso/prebuilt.tsv': b'prebuilt', 'pso/rest_pairs.tsv': b'rest', 'pso/archive.tsv': b'index',
            'env.txt': b'DM_TEST=1', 'split.tsv': b'split', 'icon.jpg': b'jpeg', 'source/launcher.c': b'launcher',
            'host/romfs_image.h': b'validator', 'host/save_sync.c': b'sync', 'host/save_sync.h': b'sync api', 'tools/romfs.py': b'reader',
            'source/Launcher.swift': b'window', 'ios/Sources/LauncherSettings.swift': b'model', 'ios/Sources/LauncherRow.swift': b'rows',
        }
        for n in pack_app.TABLES: files['profile/' + n] = n.encode()
        for n, content in files.items(): (t / n).write_bytes(content)
        for tool in ('clang', 'swiftc', 'codesign', 'other-codesign'):
            (t / 'bin' / tool).write_text('fake ' + tool); (t / 'bin' / tool).chmod(0o755)
        self.args = pack_app.parse([str(t / 'Game.app'), '--host', str(t / 'dmhost'), '--patched', str(t / 'patched'),
                                   '--romfs', str(t / 'romfs'), '--msl', str(t / 'msl'), '--tables', str(t / 'profile'), '--font', str(t / 'font'),
                                   '--aoc', str(t / 'aoc'), '--pso', str(t / 'pso'), '--pso-pairs', str(t / 'split.tsv'), '--env-file', str(t / 'env.txt'),
                                   '--title-id', TID, '--name', 'Test Game', '--marker', 'marker', '--icon', str(t / 'icon.jpg')])
        self.commands = []
        self.clang_version, self.swiftc_version, self.sdk_version = 'clang fixture 1', 'swiftc fixture 1', '27.0'
        self.clang, self.swiftc, self.sdk = t / 'bin/clang', t / 'bin/swiftc', t / 'sdk'
        self.codesign = t / 'bin/codesign'
        self.failed_tool = None
        real_run, real_which = pack_app.run, shutil.which
        def fake_run(cmd):
            cmd = [str(c) for c in cmd]; self.commands.append(cmd)
            name = Path(cmd[0]).name
            if self.failed_tool == name and (name not in ('clang', 'swiftc') or '-o' in cmd): raise SystemExit('fixture ' + name + ' failed')
            if name == 'otool': return 'cmd LC_BUILD_VERSION\n minos 14.0\n'
            if name == 'xcrun':
                if '--find' in cmd: return str(self.swiftc if 'swiftc' in cmd else self.clang) + '\n'
                if '--show-sdk-path' in cmd: return str(self.sdk) + '\n'
                if '--show-sdk-version' in cmd: return self.sdk_version + '\n'
                if 'clang' in cmd: name = 'clang'
            if name in ('clang', 'swiftc'):
                if '--version' in cmd: return (self.clang_version if name == 'clang' else self.swiftc_version) + '\n'
                out = Path(cmd[cmd.index('-o') + 1]); out.write_bytes(b'fake ' + name.encode()); out.chmod(0o755); return ''
            if name in ('codesign', 'other-codesign'): return ''
            if name == 'sips': Path(cmd[cmd.index('--out') + 1]).write_bytes(b'png'); return ''
            if name == 'iconutil': Path(cmd[cmd.index('-o') + 1]).write_bytes(b'icns'); return ''
            return real_run(cmd)
        def which(name): return str(self.codesign) if name == 'codesign' else real_which(name)
        for patch in (mock.patch.object(pack_app, 'run', fake_run), mock.patch.object(pack_app, 'HERE', t / 'source'),
                      mock.patch.object(pack_app, 'can_clone', return_value=False), mock.patch.object(shutil, 'which', which)):
            patch.start(); self.addCleanup(patch.stop)
        self.clone = pack_app.can_clone

    def pack(self):
        self.commands.clear(); self.clone.reset_mock()
        with contextlib.redirect_stdout(io.StringIO()) as output: pack_app.pack(self.args)
        return output.getvalue()

    @property
    def key(self): return Path(self.args.out + '.key')

    def assert_reused(self):
        self.assertEqual(self.pack(), f'app: {Path(self.args.out).resolve()}, reused (inputs unchanged)\n')
        self.clone.assert_not_called()
        self.assertFalse(any('-o' in c or '--sign' in c or c[0] == '/bin/cp' for c in self.commands))

    def assert_repacked(self):
        self.assertNotIn('reused (inputs unchanged)', self.pack())
        self.assertTrue(any('--sign' in c for c in self.commands))
        self.assertTrue(self.key.is_file())

    def test_retargeted_input_symlink_repacks(self):
        msl = self.tmp / 'msl'
        for name in ('left.metallib', 'right.metallib'): (msl / name).write_bytes(b'same library')
        link = msl / 'linked.metallib'; link.symlink_to('left.metallib')
        self.pack(); self.assert_reused()
        link.unlink(); link.symlink_to('right.metallib')
        self.assert_repacked()
        packed = Path(self.args.out) / 'Contents/Resources/msl/msl/linked.metallib'
        self.assertEqual(os.readlink(packed), 'right.metallib')
        self.assert_reused()

    def test_unchanged_reuses_before_partial_clone_compile_and_sign(self):
        self.pack()
        partial = Path(self.args.out + '.partial'); partial.mkdir(); (partial / 'leave').write_text('untouched')
        self.assert_reused()
        self.assertEqual((partial / 'leave').read_text(), 'untouched')

    def test_each_selected_file_content_invalidates_even_with_same_stat(self):
        self.pack()
        names = ['dmhost', 'patched/main.patched.bin', 'patched/main.fixups.tsv', 'patched/main.gates.tsv', 'patched/main.cuts.tsv',
                 'patched/main.obs.tsv', 'patched/nro.tsv', 'msl/k.metallib', 'msl/k.json', 'msl/inputs.tsv', 'font/nintendo_ext.ttf',
                 'pso/gpu.mtl4a', 'pso/prebuilt.tsv', 'pso/rest_pairs.tsv', 'pso/archive.tsv', 'env.txt', 'icon.jpg', 'source/launcher.c',
                 'source/Launcher.swift', 'ios/Sources/LauncherSettings.swift', 'ios/Sources/LauncherRow.swift',   # the settings window's sources
                 'host/save_sync.c', 'host/save_sync.h']   # the cloud-save code the launcher links (its send after the host exits)
        names += ['profile/' + n for n in pack_app.TABLES]
        for name in names:
            with self.subTest(name=name):
                p = self.tmp / name; st = p.stat(); data = p.read_bytes(); p.write_bytes(bytes([data[0] ^ 1]) + data[1:])
                os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns)); self.assert_repacked()

    def test_romfs_and_aoc_stat_trees_include_paths_modes_and_empty_directories(self):
        self.pack()
        for name in ('romfs/marker', 'aoc/1/data'):
            with self.subTest(name=name):
                p = self.tmp / name; p.write_bytes(b'changed size'); self.assert_repacked()
                p.chmod(0o600); self.assert_repacked()
                p.with_name('added').write_bytes(b'new'); self.assert_repacked()
                p.with_name('added').rename(p.with_name('renamed')); self.assert_repacked()
        (self.tmp / 'romfs/new-empty').mkdir(); self.assert_repacked()

    def test_image_is_copied_once_and_its_changes_invalidate_reuse(self):
        import romfs_image
        source = self.tmp / 'romfs'; image = self.tmp / 'RomFS.img'
        romfs_image.pack(source, image); self.args.romfs = str(image)
        self.pack(); self.assert_reused()
        resources = Path(self.args.out) / 'Contents/Resources'
        self.assertEqual((resources / 'romfs.img').read_bytes(), image.read_bytes())
        self.assertFalse((resources / 'romfs').exists())
        (source / 'marker').write_text('new bytes'); romfs_image.pack(source, image)
        self.assert_repacked()

    def test_all_metadata_sign_identity_and_selected_paths_invalidate(self):
        self.pack()
        for name, value in [('name', 'New Game'), ('version', '2.0'), ('bundle_id', 'org.test.new'), ('category', 'public.app-category.action-games'),
                            ('lang', 'ja'), ('title_id', '0100000000000000'), ('marker', 'empty'), ('msl_name', 'custom'), ('sign', 'Fixture identity')]:
            with self.subTest(option=name): setattr(self.args, name, value); self.assert_repacked()
        for name in ('host', 'patched', 'romfs', 'msl', 'tables', 'font', 'aoc', 'pso', 'env_file', 'icon', 'pso_pairs'):
            with self.subTest(path=name):
                old = Path(getattr(self.args, name)); new = old.with_name(old.name + '-other')
                if old.is_dir(): shutil.copytree(old, new)
                else: shutil.copy2(old, new)
                setattr(self.args, name, str(new)); self.assert_repacked()

    def test_compiler_sdk_and_resolved_sign_tool_invalidate(self):
        self.pack()
        self.clang_version = 'clang fixture 2'; self.assert_repacked()
        self.clang.write_text('changed compiler'); self.assert_repacked()
        self.swiftc_version = 'swiftc fixture 2'; self.assert_repacked()
        self.swiftc.write_text('changed Swift compiler'); self.assert_repacked()
        self.sdk_version = '28.0'; self.assert_repacked()
        self.sdk = self.tmp / 'other-sdk'; self.sdk.mkdir(); self.assert_repacked()
        self.codesign = self.tmp / 'bin/other-codesign'; self.assert_repacked()
        self.codesign.write_text('changed signing tool'); self.assert_repacked()

    def test_launcher_is_linked_from_its_c_and_swift_sources(self):
        self.pack()
        c = next(c for c in self.commands if Path(c[0]).name == 'clang' and '-o' in c)
        s = next(c for c in self.commands if Path(c[0]).name == 'swiftc' and '-o' in c)
        self.assertIn('-c', c); self.assertIn('-parse-as-library', s)
        self.assertEqual(s[s.index('-o') + 1], str(Path(self.args.out).resolve().with_name('Game.app.partial') / 'Contents/MacOS/nxnative'))
        self.assertIn(c[c.index('-o') + 1], s)   # launcher.c's object, with its main
        sync = next(c for c in self.commands if Path(c[0]).name == 'clang' and '-o' in c and c[-1].endswith('save_sync.c'))
        self.assertIn(sync[sync.index('-o') + 1], s)   # save_sync.c's object: the launcher sends the save once the host has exited
        self.assertEqual([Path(x).name for x in s if x.endswith('.swift')], ['Launcher.swift', 'LauncherSettings.swift', 'LauncherRow.swift'])

    def test_selected_file_modes_and_file_additions_and_removals_invalidate(self):
        self.pack()
        (self.tmp / 'dmhost').chmod(0o755); self.assert_repacked()
        for name in ('patched/new.patched.bin', 'patched/new.fixups.tsv', 'msl/new.metallib', 'font/new.ttf', 'pso/new.mtl4a'):
            (self.tmp / name).write_text('new')
        self.assert_repacked()
        dm = Path(self.args.out) / 'Contents/Resources/dm'
        self.assertEqual((dm / 'new.patched.bin').read_text(), 'new')
        (self.tmp / 'profile/c1_full.tsv').unlink(); self.assert_repacked()
        self.assertFalse((dm / 'c1_full.tsv').exists())
        (self.tmp / 'patched/new.patched.bin').unlink(); (self.tmp / 'patched/new.fixups.tsv').unlink(); self.assert_repacked()

    def test_output_path_and_optional_inputs_invalidate(self):
        self.pack()
        old = Path(self.args.out); new = old.with_name('Other.app')
        shutil.copytree(old, new); shutil.copy2(self.key, Path(str(new) + '.key'))
        self.args.out = str(new); self.assert_repacked()
        for name in ('icon', 'env_file', 'aoc', 'pso', 'pso_pairs'):
            with self.subTest(option=name): setattr(self.args, name, None); self.assert_repacked()

    def test_packer_source_content_invalidates(self):
        source = self.tmp / 'source/pack_app.py'; shutil.copy2(pack_app.__file__, source)
        with mock.patch.object(pack_app, '__file__', str(source)):
            self.pack(); source.write_text(source.read_text() + '\n# fixture change\n'); self.assert_repacked()

    def test_output_missing_modified_mode_time_path_and_directory_invalidate(self):
        self.pack()
        launcher = Path(self.args.out) / 'Contents/MacOS/nxnative'
        launcher.chmod(0o644); self.assert_repacked()
        launcher.unlink(); self.assert_repacked()
        launcher.write_bytes(b'broken'); self.assert_repacked()
        st = launcher.stat(); os.utime(launcher, ns=(st.st_atime_ns, st.st_mtime_ns + 1000000000)); self.assert_repacked()
        extra = Path(self.args.out) / 'Contents/extra'; extra.mkdir(); self.assert_repacked()
        empty = Path(self.args.out) / 'Contents/Resources/romfs/empty'
        empty.chmod(0o700); self.assert_repacked()
        empty.rename(empty.with_name('renamed')); self.assert_repacked()
        shutil.rmtree(self.args.out); self.assert_repacked()

    def test_unpacked_files_and_unselected_split_pairs_do_not_invalidate(self):
        self.pack()
        for name in ('msl/k.metal', 'msl/_b_build.json', 'msl/analysis/report.txt', 'patched/main.sites.tsv', 'profile/game.tsv',
                     'font/readme.txt', 'pso/analysis.txt', 'split.tsv'):
            p = self.tmp / name; p.parent.mkdir(exist_ok=True); p.write_text('not packed'); self.assert_reused()
        (self.tmp / 'split.tsv').unlink(); self.assert_reused()

    def test_split_pairs_fallback_is_selected_only_without_profile_pairs(self):
        (self.tmp / 'profile/pso_pairs.tsv').unlink(); self.pack()
        target = Path(self.args.out) / 'Contents/Resources/dm/pso_pairs.tsv'
        self.assertEqual(target.read_bytes(), b'split')
        (self.tmp / 'split.tsv').write_bytes(b'new split'); self.assert_repacked()
        self.assertEqual(target.read_bytes(), b'new split')

    def test_missing_selected_inputs_fail_before_reuse_or_partial_creation(self):
        self.pack()
        for name in ('dmhost', 'patched/main.fixups.tsv', 'msl/k.metallib', 'romfs/marker', 'pso/prebuilt.tsv', 'env.txt', 'icon.jpg',
                     'font', 'aoc', 'profile', 'bin/clang', 'bin/swiftc', 'sdk', 'bin/codesign', 'source/launcher.c', 'source/Launcher.swift', 'ios/Sources/LauncherRow.swift'):
            with self.subTest(name=name):
                p = self.tmp / name; hidden = p.with_name(p.name + '.hidden'); p.rename(hidden)
                try:
                    with self.assertRaises((SystemExit, OSError)): self.pack()
                    self.assertFalse(self.key.exists())
                    self.assertFalse(Path(self.args.out + '.partial').exists())
                finally: hidden.rename(p)
                self.pack()

    def test_failed_repack_drops_key_and_keeps_old_app(self):
        self.pack()
        before = (Path(self.args.out) / 'Contents/Info.plist').read_bytes()
        for tool in ('clang', 'swiftc', 'codesign'):
            with self.subTest(tool=tool):
                self.args.name += ' changed'; self.failed_tool = tool
                with self.assertRaisesRegex(SystemExit, 'fixture'): self.pack()
                self.assertFalse(self.key.exists())
                self.assertEqual((Path(self.args.out) / 'Contents/Info.plist').read_bytes(), before)
                self.failed_tool = None
        self.assert_repacked()

    def test_failed_atomic_replacement_drops_key_and_keeps_old_app(self):
        self.pack(); before = (Path(self.args.out) / 'Contents/Info.plist').read_bytes()
        self.args.name = 'changed'
        library = types.SimpleNamespace(renamex_np=mock.Mock(return_value=-1))
        with mock.patch.object(pack_app.ctypes, 'CDLL', return_value=library), mock.patch.object(pack_app.ctypes, 'get_errno', return_value=5):
            with self.assertRaises(OSError): self.pack()
        self.assertFalse(self.key.exists())
        self.assertEqual((Path(self.args.out) / 'Contents/Info.plist').read_bytes(), before)
        self.assert_repacked()

    def test_failed_key_write_rolls_back_app_and_drops_key(self):
        self.pack(); before = (Path(self.args.out) / 'Contents/Info.plist').read_bytes()
        self.args.name = 'changed'
        real_replace = Path.replace
        def fail_key(path, target):
            if str(target).endswith('.app.key'): raise OSError('fixture key write failed')
            return real_replace(path, target)
        with mock.patch.object(Path, 'replace', fail_key):
            with self.assertRaisesRegex(OSError, 'fixture key write'): self.pack()
        self.assertFalse(self.key.exists())
        self.assertFalse(Path(str(self.key) + '.partial').exists())
        self.assertEqual((Path(self.args.out) / 'Contents/Info.plist').read_bytes(), before)
        self.assert_repacked()


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('codesign'), 'needs macOS (clang, codesign, sips, iconutil)')
class App(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(dir=TEST_ROOT)).resolve()
        t = cls.tmp
        (t / 'host.c').write_text(HOST_C)
        subprocess.run(['xcrun', 'clang', '-arch', 'arm64', '-o', t / 'dmhost', t / 'host.c'], check=True)
        for d in ('patched', 'romfs/region_common/audio', 'cache/b-0123456789ab', 'profile', 'font', 'aoc/1', 'bin', 'home'): (t / d).mkdir(parents=True)
        for n, b in (('main.patched.bin', b'code'), ('main.fixups.tsv', b'f'), ('main.gates.tsv', b'g'), ('main.cuts.tsv', b'c'), ('main.obs.tsv', b'o'), ('main.changed.tsv', b'w'), ('main.sites.tsv', b's'), ('build_src.sha256', b'h')):
            (t / 'patched' / n).write_bytes(b)
        (t / 'romfs/region_common/audio/edward.bfsar').write_bytes(b'snd')
        (t / 'cache/b-0123456789ab/k_b.metallib').write_bytes(b'lib'); (t / 'cache/b-0123456789ab/inputs.tsv').write_text('# stamp\n')
        (t / 'cache/b-0123456789ab/k_b.json').write_text('{}'); (t / 'cache/b-0123456789ab/k_b.metal').write_text('msl'); (t / 'cache/b-0123456789ab/_b_build.json').write_text('[]')
        (t / 'cache/cas').mkdir()   # the Metal compile cache: not for the app
        for n in ('shader_map.tsv', 'pso_manifest.tsv', 'c1_full.tsv', 'game.tsv'): (t / 'profile' / n).write_text(n)
        (t / 'profile/shader_pairs.tsv').write_text('vertex-key\tpixel-key\n'); (t / 'profile/pso_pairs.tsv').write_text('other-vertex\tother-pixel\n')
        (t / 'font/nintendo_ext.ttf').write_bytes(b'ttf')
        (t / 'aoc/1/x').write_bytes(b'dlc')
        (t / 'pso').mkdir(); (t / 'pso/applegpu_g14s.mtl4a').write_bytes(b'arch'); (t / 'pso/prebuilt.tsv').write_text('chain\t0123456789ab\n'); (t / 'pso/key.x').write_text('not for the app')
        (t / 'env.txt').write_text('# baked in\nDM_BAKED=1\nDM_BOTH=bundle\n')
        (t / 'launcher.json').write_text('{"game": "x", "settings": []}\n')
        subprocess.run(['sips', '-s', 'format', 'jpeg', '-z', '256', '256', '/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericApplicationIcon.icns',
                        '--out', t / 'icon.jpg'], check=True, capture_output=True)
        (t / 'bin/osascript').write_text('#!/bin/sh\necho "$@" > "$HOME/alert.txt"\nprintf "%s\\0" "$@" > "$HOME/alert.args"\n'
                                       '/usr/bin/osacompile -o "$HOME/alert.scpt" "$1" "$2" 2> "$HOME/alert.error"\n')
        (t / 'bin/osascript').chmod(0o755)
        cls.app = t / 'Game - Test.app'
        cls.args = pack_app.parse([str(cls.app), '--host', str(t / 'dmhost'), '--patched', str(t / 'patched'), '--romfs', str(t / 'romfs'), '--msl', str(t / 'cache/b-0123456789ab'),
                                   '--tables', str(t / 'profile'), '--font', str(t / 'font'), '--aoc', str(t / 'aoc'), '--pso', str(t / 'pso'), '--env-file', str(t / 'env.txt'), '--settings', str(t / 'launcher.json'), '--title-id', TID.lower(),
                                   '--name', 'Game: Test', '--version', '1.0.1', '--lang', 'fr', '--marker', 'region_common/audio/edward.bfsar', '--icon', str(t / 'icon.jpg'),
                                   '--category', 'public.app-category.games'])
        with contextlib.redirect_stdout(io.StringIO()): pack_app.pack(cls.args)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp)

    def launch(self, wait=False, app=None, args=(), **env):
        home = self.tmp / 'home'
        e = dict(HOME=str(home), PATH=f'{self.tmp / "bin"}:/usr/bin:/bin', **{'DM_LAUNCHER': '0', **env})   # never the settings window in a test
        if wait: e['STANDIN_WAIT'] = '1'
        return subprocess.Popen([(app or self.app) / 'Contents/MacOS/nxnative', *args], env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def host_run(self):
        out = {}
        for ln in (self.tmp / 'home/host_run.txt').read_text().splitlines():
            k, _, v = ln.partition('='); out[k] = v
        return out

    def test_layout(self):
        c = self.app / 'Contents'
        self.assertFalse(self.app.with_name(self.app.name + '.partial').exists())
        self.assertEqual(sorted(p.name for p in (c / 'MacOS').iterdir()), ['dmhost', 'nxnative'])
        self.assertEqual(sorted(p.name for p in (c / 'Resources/dm').iterdir()),
                         ['c1_full.tsv', 'env.txt', 'font', 'launcher.json', 'main.changed.tsv', 'main.cuts.tsv', 'main.fixups.tsv', 'main.gates.tsv', 'main.obs.tsv', 'main.patched.bin', 'pso_manifest.tsv', 'pso_pairs.tsv',
                          'shader_map.tsv', 'shader_pairs.tsv'])   # the shader pairs: the pipeline prebuild's, without which hybrid draws have no stand-in
        self.assertEqual([p.name for p in (c / 'Resources/msl').iterdir()], ['b-0123456789ab'])   # the libraries only, not the compile cache
        self.assertEqual(sorted(p.name for p in (c / 'Resources/msl/b-0123456789ab').iterdir()), ['inputs.tsv', 'k_b.json', 'k_b.metallib'])
        self.assertEqual((c / 'Resources/msl/b-0123456789ab/k_b.metallib').read_bytes(), b'lib')
        self.assertTrue((c / 'Resources/romfs/region_common/audio/edward.bfsar').is_file())
        self.assertTrue((c / 'Resources/aoc/1/x').is_file()); self.assertTrue((c / 'Resources/dm/font/nintendo_ext.ttf').is_file())
        self.assertEqual(sorted(p.name for p in (c / 'Resources/pso').iterdir()), ['applegpu_g14s.mtl4a', 'prebuilt.tsv'])   # the pipeline archive and its stamp only
        self.assertEqual((c / 'Resources/AppIcon.icns').read_bytes()[:4], b'icns')

    def test_pipeline_pair_tables(self):   # the pipeline prebuild reads them beside DM_NVN_SHMAP: without them it builds nothing, though every shader library is there
        dm = self.app / 'Contents/Resources/dm'
        self.assertEqual((dm / 'shader_pairs.tsv').read_text(), 'vertex-key\tpixel-key\n')
        self.assertEqual((dm / 'pso_pairs.tsv').read_text(), 'other-vertex\tother-pixel\n')

    def test_split_pso_pairs(self):   # a profile without pso_pairs.tsv: the shader split's copy (--pso-pairs); the profile's own wins when it has one
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as d, mock.patch.object(pack_app, 'sign', lambda *a: None), mock.patch('sys.stdout'):
            d = Path(d); (d / 'profile').mkdir(); (d / 'profile/shader_map.tsv').write_text('m'); (d / 'split.tsv').write_text('split-vertex\tsplit-pixel\n')
            a = types.SimpleNamespace(**vars(self.args)); a.out = str(self.tmp / 'Split.app'); a.icon = None; a.tables = str(d / 'profile'); a.pso_pairs = str(d / 'split.tsv')
            pack_app.pack(a)
            self.assertEqual((self.tmp / 'Split.app/Contents/Resources/dm/pso_pairs.tsv').read_text(), 'split-vertex\tsplit-pixel\n')
            a.out = str(self.tmp / 'Own.app'); a.tables = self.args.tables; pack_app.pack(a)
            self.assertEqual((self.tmp / 'Own.app/Contents/Resources/dm/pso_pairs.tsv').read_text(), 'other-vertex\tother-pixel\n')

    def test_info_plist(self):
        p = plistlib.loads((self.app / 'Contents/Info.plist').read_bytes())
        self.assertEqual((p['CFBundleExecutable'], p['CFBundleName'], p['CFBundleIdentifier'], p['CFBundleShortVersionString']), ('nxnative', 'Game: Test', 'org.nxnative.01008cf01baac000', '1.0.1'))
        self.assertEqual((p['DMTitleID'], p['DMLanguage'], p['DMRomfsMarker'], p['CFBundleIconFile']), (TID, 'fr', 'region_common/audio/edward.bfsar', 'AppIcon'))
        self.assertEqual(p['DMSettings'], [])   # launcher.json's settings, as the saved choices they accept: none here
        self.assertRegex(p['DMCommit'], r'^[0-9a-f]{12}(\+dirty)?$')   # the packing checkout (scripts/bug_report.py, the host's start line)
        self.assertEqual(p['DMChain'], '0123456789ab')   # the chain of the libraries packed (msl/b-<chain>)

    def test_packed_language_is_the_lists(self):   # --lang a game lacks: the Game language row's default (settings.py: the game's first), with or without the window
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as d:
            listed = Path(d) / 'launcher.json'
            listed.write_text(json.dumps({'game': TID, 'settings': [{'env': 'DM_LANG', 'kind': 'choice', 'section': 'Game', 'label': 'Game language', 'note': '', 'default': 'ja',
                                                                      'options': [['ja', 'Japanese'], ['ko', 'Korean']]}]}))
            a = types.SimpleNamespace(**vars(self.args)); a.settings = str(listed)
            self.assertEqual(pack_app.info_plist(a, '14.0', None)['DMLanguage'], 'ja')   # --lang fr
            a.settings = None
            self.assertEqual(pack_app.info_plist(a, '14.0', None)['DMLanguage'], 'fr')   # no list: --lang as given

    def test_saved_choices(self):
        """launcher.<VAR> in the app's preferences domain, as a front end or a settings window saves it: used from Finder or the Dock, checked against this build's settings,
        after the launch environment, the player's env.txt and the bundle's, and ignored with DM_SAVED_CHOICES=0"""
        domain = f'org.nxnative.test.{uuid.uuid4().hex}'   # never a real game's domain
        listed = {'schema': 1, 'game': TID, 'settings': [
            {'env': 'DM_FPS_TARGET', 'kind': 'choice', 'section': 'Game', 'label': 'Frame rate', 'note': '', 'default': '60', 'options': [['30', '30 fps'], ['60', '60 fps']]},
            {'env': 'DM_RENDER_SCALE', 'kind': 'range', 'section': 'Game', 'label': 'Render scale', 'note': '', 'default': '1.0', 'min': 0.5, 'max': 1.0, 'step': 0.05},
            {'env': 'DM_FPS_HOLD', 'kind': 'toggle', 'section': 'Performance', 'label': 'Hold', 'note': '', 'default': 'on', 'on': 'on', 'off': 'off'},
            {'env': 'DM_LANG', 'kind': 'choice', 'section': 'Game', 'label': 'Language', 'note': '', 'default': 'fr', 'options': [['fr', 'French'], ['ja', 'Japanese']]},
            {'env': 'DM_BOTH', 'kind': 'choice', 'section': 'Game', 'label': 'Both', 'note': '', 'default': 'bundle', 'options': [['bundle', 'b'], ['player', 'p'], ['saved', 's']]},
            {'env': 'DM_BAKED', 'kind': 'toggle', 'section': 'Game', 'label': 'Baked', 'note': '', 'default': '1', 'on': '1', 'off': '0'}]}
        sup = self.tmp / f'home/Library/Application Support/nxnative/{TID}'; sup.mkdir(parents=True, exist_ok=True); (sup / 'env.txt').write_text('DM_BOTH=player\n')

        def pack(name, settings):
            (self.tmp / f'{name}.json').write_text(json.dumps(settings))
            a = types.SimpleNamespace(**vars(self.args)); a.out = str(self.tmp / f'{name}.app'); a.icon = None; a.bundle_id = domain; a.settings = str(self.tmp / f'{name}.json')
            with mock.patch.object(pack_app, 'sign', lambda *x: None), mock.patch('sys.stdout'): pack_app.pack(a)
            return self.tmp / f'{name}.app'

        def run(app, args=(), **env):
            p = self.launch(app=app, args=args, **env); out = p.communicate(timeout=30)[0].decode()
            self.assertEqual(p.returncode, 7, out)
            return self.host_run()

        def save(key, *value): subprocess.run(['defaults', 'write', domain, f'launcher.{key}', *value], check=True)
        try:
            app = pack('Saved', listed)
            self.assertEqual(plistlib.loads((app / 'Contents/Info.plist').read_bytes())['DMSettings'][:3],
                             [{'env': 'DM_FPS_TARGET', 'values': ['30', '60']}, {'env': 'DM_RENDER_SCALE', 'min': 0.5, 'max': 1.0}, {'env': 'DM_FPS_HOLD', 'values': ['on', 'off']}])
            for key, value in [('DM_FPS_TARGET', '30'), ('DM_RENDER_SCALE', '0.75'), ('DM_FPS_HOLD', 'maybe'), ('DM_LANG', 'ja'), ('DM_BOTH', 'saved'), ('DM_BAKED', '0')]: save(key, '-string', value)
            save('DM_OPMODE', '-string', '0')   # not one of this build's settings
            r = run(app)   # from Finder or the Dock: no environment of its own
            self.assertEqual((r['DM_FPS_TARGET'], r['DM_RENDER_SCALE'], r.get('DM_FPS_HOLD')), ('30', '0.75', None))   # a value the toggle never takes is ignored
            self.assertEqual(r['DM_LANG'], 'ja')   # a saved choice before the packed language
            self.assertEqual((r['DM_BOTH'], r['DM_BAKED']), ('player', '1'))   # the player's env.txt and the bundle's win over a saved choice
            self.assertNotIn('DM_OPMODE', r)
            self.assertEqual((r['DM_ENV_FILE_KEYS'], r['DM_ENV_LAUNCH_KEYS']), ('DM_BOTH DM_BAKED', ''))   # what the settings window keeps: the env files' variables
            r = run(app, DM_FPS_TARGET='60', DM_RENDER_SCALE='0.5')   # a front end's launch overrides: they win, and are not saved
            self.assertEqual((r['DM_FPS_TARGET'], r['DM_RENDER_SCALE']), ('60', '0.5'))
            self.assertEqual(r['DM_ENV_LAUNCH_KEYS'], 'DM_FPS_TARGET DM_RENDER_SCALE')   # the window shows them fixed ("Set at launch")
            r = run(app, args=['--no-launcher'], DM_LAUNCHER='1')   # open <app> --args --no-launcher: no window, the saved choices
            self.assertEqual(r['DM_FPS_TARGET'], '30')
            self.assertEqual(subprocess.run(['defaults', 'read', domain, 'launcher.DM_FPS_TARGET'], capture_output=True, text=True).stdout.strip(), '30')
            r = run(app, DM_SAVED_CHOICES='0')
            self.assertEqual((r.get('DM_FPS_TARGET'), r.get('DM_RENDER_SCALE'), r['DM_LANG']), (None, None, 'fr'))
            save('DM_RENDER_SCALE', '-float', '0.75')   # not a string
            self.assertNotIn('DM_RENDER_SCALE', run(app))
            save('DM_RENDER_SCALE', '-string', ' 0.75')   # a number the iPhone's settings screen would not take either
            self.assertNotIn('DM_RENDER_SCALE', run(app))
            save('DM_RENDER_SCALE', '-string', '0x1p-1')   # hex, which neither side takes (Setting.accepts)
            self.assertNotIn('DM_RENDER_SCALE', run(app))
            listed['settings'][0]['options'] = [['60', '60 fps']]; listed['settings'][0]['default'] = '60'   # a rebuild that no longer offers 30 fps
            r = run(pack('Rebuilt', listed))
            self.assertEqual((r.get('DM_FPS_TARGET'), r['DM_LANG']), (None, 'ja'))   # the host's tested default; the other saved choices still hold
            self.assertIn('saved choice DM_FPS_TARGET=30 ignored', (self.tmp / f'home/Library/Logs/nxnative/{TID}.log').read_text())
        finally:
            subprocess.run(['defaults', 'delete', domain], capture_output=True)
            Path(os.path.expanduser(f'~/Library/Preferences/{domain}.plist')).unlink(missing_ok=True)

    def test_signed(self):   # ad-hoc, both the host and the bundle, never the hardened runtime (it refuses the executable memory the host makes for the game's code)
        for target in (self.app, self.app / 'Contents/MacOS/dmhost'):
            subprocess.run(['codesign', '--verify', '--strict', target], check=True, capture_output=True)
            d = subprocess.run(['codesign', '-dv', target], capture_output=True, text=True).stderr
            self.assertIn('Signature=adhoc', d); self.assertNotIn('runtime', d.split('CodeDirectory', 1)[1].split('\n')[0])

    def test_launch(self):
        home = self.tmp / 'home'; sup = home / f'Library/Application Support/nxnative/{TID}'; logs = home / 'Library/Logs/nxnative'
        sup.mkdir(parents=True, exist_ok=True); (sup / 'env.txt').write_text('DM_BOTH=player\n')
        p = self.launch(DM_LANG='ja'); p.communicate(timeout=30)
        self.assertEqual(p.returncode, 7)   # the host's exit code
        r, res = self.host_run(), self.app / 'Contents/Resources'
        self.assertEqual(r['argv0'], str(self.app / 'Contents/MacOS/dmhost')); self.assertEqual(r['argv1'], str(res / 'dm'))
        self.assertEqual((r['DM_ROMFS'], r['DM_MSL_DIR'], r['DM_NVN_SHMAP'], r['DM_FONT_DIR'], r['DM_AOC_DIR']),
                         (str(res / 'romfs'), str(res / 'msl'), str(res / 'dm/shader_map.tsv'), str(res / 'dm/font'), str(res / 'aoc')))
        self.assertEqual(r['DM_PSO_BUNDLE'], str(res / 'pso'))   # the pipeline archive made offline, as the iOS app's main.m sets it
        self.assertEqual((r['DM_SAVE'], r['DM_TEMP'], r['DM_ROOT'], r['cwd']), (str(sup / 'save'), str(sup / 'temp'), str(sup), str(sup)))   # pso_user.tsv goes beside save/
        self.assertEqual((r['DM_WINDOW'], r['DM_AUDIO'], r['DM_LANG']), ('1', '1', 'ja'))   # the caller's DM_LANG wins over the packed language
        self.assertEqual((r['DM_BAKED'], r['DM_BOTH']), ('1', 'player'))   # the player's env.txt before the bundle's
        self.assertTrue((sup / 'save').is_dir()); self.assertTrue((sup / 'temp').is_dir())
        self.assertIn('standin host output', (logs / f'{TID}.log').read_text())
        p = self.launch(); p.communicate(timeout=30)
        self.assertIn('standin host output', (logs / f'{TID}.prev.log').read_text())
        self.assertEqual(self.host_run()['DM_LANG'], 'fr')   # Info.plist DMLanguage

    def test_sigterm(self):   # forwarded to the host, whose exit code comes back
        p = self.launch(wait=True)
        for _ in range(100):
            if (self.tmp / 'home/host_run.txt').exists() and 'STANDIN_WAIT' in self.host_run(): break
            time.sleep(0.05)
        time.sleep(0.2)
        p.send_signal(signal.SIGTERM); p.communicate(timeout=30)
        self.assertEqual(p.returncode, 3)

    def test_missing_data(self):
        broken = self.tmp / 'Broken.app'; shutil.rmtree(broken, ignore_errors=True)
        subprocess.run(['/bin/cp', '-R', self.app, broken], check=True)
        shutil.rmtree(broken / 'Contents/Resources/romfs')
        (self.tmp / 'home/alert.txt').unlink(missing_ok=True)
        p = subprocess.run([broken / 'Contents/MacOS/nxnative'], env=dict(HOME=str(self.tmp / 'home'), PATH=f'{self.tmp / "bin"}:/usr/bin:/bin'), capture_output=True, text=True, timeout=30)
        self.assertEqual(p.returncode, 1)
        self.assertIn('game data inside this app is missing', (self.tmp / 'home/alert.txt').read_text())

    def test_missing_modules(self):
        for missing in (('main.patched.bin',), ('main.fixups.tsv',), ('main.patched.bin', 'main.fixups.tsv')):
            with self.subTest(missing=missing):
                broken = self.tmp / 'Missing module.app'; shutil.rmtree(broken, ignore_errors=True)
                shutil.copytree(self.app, broken)
                for name in missing: (broken / 'Contents/Resources/dm' / name).unlink()
                home = self.tmp / 'home'
                for name in ('alert.txt', 'host_run.txt'): (home / name).unlink(missing_ok=True)
                p = subprocess.run([broken / 'Contents/MacOS/nxnative'], env=dict(HOME=str(home), PATH=f'{self.tmp / "bin"}:/usr/bin:/bin'),
                                   capture_output=True, text=True, timeout=30)
                self.assertEqual(p.returncode, 1)
                self.assertFalse((home / 'host_run.txt').exists())
                self.assertIn('game data inside this app is missing', (home / 'alert.txt').read_text())

    def test_alert_title(self):
        broken = self.tmp / 'Quoted name.app'; shutil.copytree(self.app, broken)
        title = 'Game "Test" \\ Edition'
        plist = broken / 'Contents/Info.plist'
        info = plistlib.loads(plist.read_bytes()); info['CFBundleName'] = title; plist.write_bytes(plistlib.dumps(info))
        shutil.rmtree(broken / 'Contents/Resources/romfs')
        home = self.tmp / 'home'
        (home / 'alert.scpt').unlink(missing_ok=True)
        p = subprocess.run([broken / 'Contents/MacOS/nxnative'], env=dict(HOME=str(home), PATH=f'{self.tmp / "bin"}:/usr/bin:/bin'),
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(p.returncode, 1)
        self.assertTrue((home / 'alert.scpt').exists(), (home / 'alert.error').read_text())
        args = (home / 'alert.args').read_bytes().decode().split('\0')[:-1]
        self.assertEqual(args[2], title)   # data is passed separately from the compiled script
        self.assertIn('game data inside this app is missing', args[3])

    def test_relocated(self):   # run with the entire build folder unavailable
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as dst:
            dst = Path(dst).resolve(); moved = dst / self.app.name; home = dst / 'home'; home.mkdir()
            self.app.rename(moved)
            hidden = self.tmp.with_name(self.tmp.name + '.unavailable')
            try:
                self.tmp.rename(hidden)
                p = subprocess.run([moved / 'Contents/MacOS/nxnative'], env=dict(HOME=str(home), PATH='/usr/bin:/bin'), capture_output=True, timeout=30)
                self.assertEqual(p.returncode, 7)
                ran = (home / 'host_run.txt').read_text()
                self.assertIn(f'argv1={moved}/Contents/Resources/dm\n', ran)
                self.assertIn(f'DM_MSL_DIR={moved}/Contents/Resources/msl\n', ran)
                self.assertNotIn(str(self.tmp), ran)
            finally:
                if hidden.exists(): hidden.rename(self.tmp)
                moved.rename(self.app)

    def test_pack_missing_fixups(self):
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as src:
            src = Path(src); (src / 'main.patched.bin').write_bytes(b'code')
            a = types.SimpleNamespace(**vars(self.args)); a.patched = str(src); a.out = str(self.tmp / 'Incomplete.app')
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(SystemExit, 'no rewriter fixups'):
                pack_app.pack(a)
            self.assertFalse(Path(a.out).exists())

    def test_pack_bad_archive(self):   # a pipeline archive folder without its archive or stamp is refused before anything is copied
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as src:
            src = Path(src); (src / 'prebuilt.tsv').write_text('chain\t0123456789ab\n')
            a = types.SimpleNamespace(**vars(self.args)); a.pso = str(src); a.out = str(self.tmp / 'NoArchive.app')
            with contextlib.redirect_stdout(io.StringIO()) as o, self.assertRaisesRegex(SystemExit, 'no pipeline archive with its prebuilt.tsv'):
                pack_app.pack(a)
            self.assertEqual(o.getvalue(), ''); self.assertFalse(Path(a.out).exists())

    def test_copies_count_archive(self):   # another volume: the archive's bytes count in the free-space check, which runs before anything is copied
        a = types.SimpleNamespace(**vars(self.args)); a.out = str(self.tmp / 'Full.app')
        msls = sum(p.stat().st_size for pat in pack_app.MSL_FILES for p in Path(a.msl).glob(pat) if p.is_file() and p.name != '_b_build.json')
        pso = sum(p.stat().st_size for pat in pack_app.PSO_FILES for p in Path(a.pso).glob(pat))
        with mock.patch.object(pack_app, 'can_clone', lambda *a: False), mock.patch.object(pack_app, 'tree_bytes', lambda d: 0), \
             mock.patch.object(shutil, 'disk_usage', lambda p: types.SimpleNamespace(free=(1 << 30) + msls + pso - 1)), contextlib.redirect_stdout(io.StringIO()) as o:
            with self.assertRaisesRegex(SystemExit, 'the app needs'): pack_app.pack(a)
        self.assertEqual(o.getvalue(), ''); self.assertFalse(Path(a.out).exists())

    def test_each_source_probed(self):   # the RomFS on the app's volume, the rest on another: only the rest counts in the free-space check
        a = types.SimpleNamespace(**vars(self.args)); a.out = str(self.tmp / 'Split.app')
        romfs, probed, counted = (self.tmp / 'romfs').resolve(), [], []
        def can_clone(src, dst): probed.append(src); return src == romfs
        def tree_bytes(d): counted.append(d); return 0
        lists = sum(p.stat().st_size for d, pats in (('patched', pack_app.MODULE_FILES), ('msl', pack_app.MSL_FILES), ('pso', pack_app.PSO_FILES))
                    for pat in pats for p in (self.tmp / d).glob(pat) if p.is_file() and p.name != '_b_build.json')
        with mock.patch.object(pack_app, 'can_clone', can_clone), mock.patch.object(pack_app, 'tree_bytes', tree_bytes), \
             mock.patch.object(shutil, 'disk_usage', lambda p: types.SimpleNamespace(free=(1 << 30) + lists - 1)), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(SystemExit, 'the app needs'): pack_app.pack(a)
        self.assertEqual(len(probed), 5); self.assertEqual(counted, [(self.tmp / 'aoc').resolve()])

    def test_copies(self):   # another volume (no APFS clone): plain copies, the same layout; a new app replaces the old one whole
        with mock.patch.object(pack_app, 'can_clone', lambda *a: False), mock.patch.object(pack_app, 'sign', lambda *a: None), mock.patch('sys.stdout'):
            a = types.SimpleNamespace(**vars(self.args)); a.out = str(self.tmp / 'Copy.app'); a.icon = None
            (self.tmp / 'Copy.app/Contents/stale').mkdir(parents=True)
            pack_app.pack(a)
        self.assertTrue((self.tmp / 'Copy.app/Contents/Resources/romfs/region_common/audio/edward.bfsar').is_file())
        self.assertFalse((self.tmp / 'Copy.app/Contents/stale').exists()); self.assertFalse((self.tmp / 'Copy.app/Contents/Resources/AppIcon.icns').exists())


class Script(unittest.TestCase):
    def test_options(self):
        a = nsp.parse_args(['--platform', 'mac-app', '--mac-sign', 'Developer ID Application: X (T)', '-o', '/tmp/x.app'])
        self.assertEqual((a.platform, a.mac_sign, a.out), ('mac-app', 'Developer ID Application: X (T)', '/tmp/x.app'))

    def test_app_name(self):
        self.assertEqual(nsp.mac_app_name('The Legend of Zelda: Echoes of Wisdom'), 'The Legend of Zelda - Echoes of Wisdom')
        self.assertEqual(nsp.mac_app_name('AC/DC'), 'AC - DC'); self.assertEqual(nsp.mac_app_name(':'), 'nxnative')

    def pack_cmd(self, gfx, **kw):
        ran, tmp = [], Path(tempfile.mkdtemp(dir=TEST_ROOT)); self.addCleanup(shutil.rmtree, tmp); self.said = []; self.settings_env = {}
        if kw.get('work'): tmp = kw['work']
        out = tmp / 'G.app'; shutil.rmtree(out, ignore_errors=True)
        def fake_run(cmd, log, env=None, cwd=None, on_line=None):
            ran.append([str(c) for c in cmd])
            if str(cmd[1]).endswith('dm/games/settings.py'):   # the launcher settings, before the packer
                self.settings_env = env; Path(cmd[-1]).write_text('{}'); return
            if not on_line:   # pso_offline.py: its archive, or none when every eligible pair has function constants
                o = Path(cmd[cmd.index('--out') + 1]); o.mkdir(parents=True, exist_ok=True)
                if not kw.get('no_bases'): (o / f'{cmd[cmd.index("--gpu") + 1]}.mtl4a').write_bytes(b'arch')
                return
            (out / 'Contents/MacOS').mkdir(parents=True); (out / 'Contents/MacOS/dmhost').write_bytes(b'x')
            on_line('step: launcher and host (clones of the game files)')
            on_line(f'app: {out}')
        g = types.SimpleNamespace(table=lambda n: tmp / 'profile' / n, get={'app_category': 'public.app-category.games', 'romfs_marker': 'a/b'}.get)
        g.id = '01008CF01BAAC000'
        with mock.patch.object(nsp, 'run', fake_run), mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'chain_of', lambda repo: '0123456789ab'), \
             mock.patch.object(nsp, 'mac_gpu_arch', lambda *a: 'applegpu_g14s'), mock.patch.object(nsp, 'say', self.said.append):
            nsp.mac_app(sys.executable, REPO, out, 0x01008CF01BAAC000, tmp, tmp / 'ex', tmp / 'patched', tmp / 'dmhost', tmp / 'font', tmp / 'shader/cache' if gfx == 'nvn' else tmp / 'shader',
                        kw.get('aoc'), gfx, 'en-US', g, 'Game', '1.0.0', 'org.nxnative.01008cf01baac000', None, kw.get('sign', ''), {}, tmp, gpus=kw.get('gpus', ''))
        self.pso_cmd = next((c for c in ran if c[1].endswith('pso_offline.py')), None)   # before the packer
        self.settings_cmd = next(c for c in ran if c[1].endswith('dm/games/settings.py'))
        c = ran[-1]; return {c[i]: c[i + 1] for i in range(3, len(c) - 1) if c[i].startswith('--')}, tmp

    def test_pack_command(self):
        o, tmp = self.pack_cmd('nvn')
        self.assertNotIn('--pso-pairs', o)   # no split copy
        (tmp / 'shader').mkdir(); (tmp / 'shader/pso_pairs.tsv').write_text('v\tf\n')
        o, tmp2 = self.pack_cmd('nvn', work=tmp)
        self.assertEqual(o['--pso-pairs'], str(tmp / 'shader/pso_pairs.tsv'))   # the profile has no pso_pairs.tsv: the split's copy goes in
        o, tmp = self.pack_cmd('nvn')
        self.assertEqual((o['--msl'], o['--romfs'], o['--tables'], o['--title-id'], o['--sign'], o['--marker']),
                         (str(tmp / 'shader/cache/b-0123456789ab'), str(tmp / 'ex/RomFS'), str(tmp / 'profile'), TID, '-', 'a/b'))
        self.assertNotIn('--aoc', o); self.assertNotIn('--icon', o)
        self.assertNotIn('--pso', o)   # no shader pairs: no pipeline archive
        self.assertEqual(o['--settings'], str(tmp / 'app/launcher.json'))   # the launcher settings the Mac front end reads, without the iPhone's own switches
        self.assertEqual(self.settings_cmd[2:], ['01008CF01BAAC000', str(tmp / 'patched'), str(tmp / 'ex/control.nacp'), str(tmp / 'app/launcher.json')])
        self.assertEqual((self.settings_env['SETTINGS_PLATFORM'], self.settings_env['GFXAPI'], self.settings_env['DM_LANG']), ('mac', 'nvn', 'en-US'))
        o, tmp = self.pack_cmd('vulkan', aoc=Path('/x/aoc'), sign='Developer ID Application: X (T)')
        self.assertEqual((o['--msl'], o['--aoc'], o['--sign']), (str(tmp / 'shader/vk'), '/x/aoc', 'Developer ID Application: X (T)'))

    def test_pso_archive(self):   # an NVN game with shader pairs: its pipeline archive for this Mac's GPU, made by pso_offline.py, goes into the app
        tmp = Path(tempfile.mkdtemp(dir=TEST_ROOT)); self.addCleanup(shutil.rmtree, tmp)
        (tmp / 'profile').mkdir(); (tmp / 'profile/shader_pairs.tsv').write_text('v\tf\n')
        o, _ = self.pack_cmd('nvn', work=tmp)
        self.assertEqual(o['--pso'], str(tmp / 'pso/mac'))
        p = self.pso_cmd; self.assertTrue(p[1].endswith('dm/gfx/tools/pso_offline.py'))
        self.assertEqual({p[i]: p[i + 1] for i in range(2, len(p) - 1) if p[i].startswith('--')},
                         {'--game': '01008CF01BAAC000', '--msl': str(tmp / 'shader/cache/b-0123456789ab'), '--out': str(tmp / 'pso/mac'), '--work': str(tmp / 'pso/mac-work'),
                          '--gpu': 'applegpu_g14s', '--platform': 'macos'})
        self.assertFalse(any('does not name this Mac' in m for m in self.said))
        o, _ = self.pack_cmd('nvn', work=tmp, gpus='g18p')   # PSO_GPUS set for the IPA: built as asked, with a note that this Mac's app builds its own bases
        self.assertEqual(o['--pso'], str(tmp / 'pso/mac'))
        self.assertTrue(any('does not name this Mac\'s GPU (applegpu_g14s)' in m for m in self.said))
        o, _ = self.pack_cmd('nvn', work=tmp, gpus='applegpu_g14s,g18p'); self.assertFalse(any('does not name this Mac' in m for m in self.said))
        o, _ = self.pack_cmd('nvn', work=tmp, gpus='none')
        self.assertNotIn('--pso', o); self.assertIsNone(self.pso_cmd)   # --gpu none: no archive, the app builds its bases on its first start
        shutil.rmtree(tmp / 'pso')
        o, _ = self.pack_cmd('nvn', work=tmp, no_bases=True)   # every eligible pair has function constants: pso_offline.py makes no archive, the app is packed without one
        self.assertNotIn('--pso', o); self.assertIsNotNone(self.pso_cmd)


if __name__ == '__main__':
    unittest.main()
