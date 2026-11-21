"""NSP script: NSZ / XCZ files in the NSP folder are listed by their ticket like NSPs (scan_nsps) and converted once with nsz -D into <build folder>/nsp/ (decompress_nsz):
a conversion is moved in only when nsz exits 0 and made the file, and reused while its source is unchanged. nsz itself is stood in for (no game files, keys or nsz needed)."""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)

APP = 0x010020D01AD24000


def pfs0(path, names):   # a PFS0 holding empty files with these names (enough for title_of)
    strs = b''.join(n.encode() + b'\0' for n in names); ent, o = b'', 0
    for n in names: ent += struct.pack('<QQII', 0, 0, o, 0); o += len(n) + 1
    path.write_bytes(b'PFS0' + struct.pack('<III', len(names), len(strs), 0) + ent + strs)


class Nsz(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.base = self.tmp / 'Game [v0].nsz'; pfs0(self.base, [f'{APP:016x}0000000000000011.tik', 'a.ncz'])
        self.upd = self.tmp / 'Game [v65536].nsz'; pfs0(self.upd, [f'{APP | 0x800:016x}0000000000000011.tik', 'b.ncz'])
        self.keys = self.tmp / 'custom keys' / 'console.keys'; self.keys.parent.mkdir(); self.keys.write_text('test keys\n')
        self.runs, self.fail_conversion = [], False
        for cm in (mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', lambda m: None), mock.patch.object(nsp, 'run', self.fake_nsz),
                   mock.patch.object(importlib.util, 'find_spec', lambda m, *a: object() if m == 'nsz' else None)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def fake_nsz(self, cmd, log, env=None, cwd=None, on_line=None):   # nsz -D -o OUT SRC, with keys.txt available before import; or a partial output and a non-zero exit
        src, out = Path(cmd[-1]), Path(cmd[cmd.index('-o') + 1])
        self.assertTrue(src.is_absolute())
        self.assertTrue(out.is_absolute())
        if cmd[0] == nsp.sys.executable:
            self.assertNotIn('--keys', cmd)   # the module: nsz 4.6.1's argument parser rejects it
            self.assertEqual(cmd[2], nsp.NSZ_MAIN)
        else: self.assertEqual([str(x) for x in cmd[1:3]], ['--keys', str(self.keys.resolve())])   # the standalone executable finds no keys in cwd
        self.assertEqual(Path(cwd), out)
        staged = Path(cwd) / 'keys.txt'
        self.assertTrue(staged.is_symlink())
        self.assertEqual(staged.resolve(), self.keys.resolve())
        self.assertEqual(staged.read_text(), 'test keys\n')
        self.runs.append(src.name)
        (out / (src.stem + nsp.COMPRESSED[src.suffix])).write_bytes(b'partial' if self.fail_conversion else b'nsp of ' + src.name.encode())
        if self.fail_conversion: raise nsp.BuildError('nsz exit code 1')

    def test_scan_and_convert(self):
        app, fam = nsp.scan_nsps(self.tmp)
        self.assertEqual((app, fam['base'], fam['updates']), (APP, self.base, [(65536, self.upd)]))
        fam = nsp.decompress_nsz(self.tmp, app, fam, self.keys, {})
        out = self.tmp / f'{APP:016X}_build' / 'nsp'
        self.assertEqual((fam['base'], fam['updates']), (out / 'Game [v0].nsp', [(65536, out / 'Game [v65536].nsp')]))
        self.assertEqual(fam['base'].read_bytes(), b'nsp of Game [v0].nsz')
        self.assertFalse((out / 'partial').exists())
        nsp.decompress_nsz(self.tmp, app, nsp.scan_nsps(self.tmp)[1], self.keys, {})   # unchanged sources: reused
        self.assertEqual(len(self.runs), 2)
        os.utime(self.upd, ns=(0, 0))   # a changed source: converted again
        nsp.decompress_nsz(self.tmp, app, nsp.scan_nsps(self.tmp)[1], self.keys, {})
        self.assertEqual(self.runs[2:], [self.upd.name])

    def test_work_dir(self):   # --work-dir: the conversions go to the build folder named, nothing to the NSP folder
        app, fam = nsp.scan_nsps(self.tmp)
        work = self.tmp / 'builds' / 'game'
        fam = nsp.decompress_nsz(self.tmp, app, fam, self.keys, {}, work)
        self.assertEqual(fam['base'], work / 'nsp' / 'Game [v0].nsp')
        self.assertTrue((work / 'nsp' / 'Game [v0].nsp.key').is_file())
        self.assertFalse((self.tmp / f'{APP:016X}_build').exists())

    def test_relative_paths(self):
        previous_cwd = Path.cwd(); self.addCleanup(os.chdir, previous_cwd); os.chdir(self.tmp)
        app, fam = nsp.scan_nsps(Path('.'))
        fam = nsp.decompress_nsz(Path('.'), app, fam, self.keys.relative_to(self.tmp), {})
        self.assertEqual(fam['base'].resolve(), (self.tmp / f'{APP:016X}_build/nsp/Game [v0].nsp').resolve())
        self.assertFalse((fam['base'].parent / 'partial').exists())

    def test_xcz_uses_the_selected_keys(self):
        self.base.rename(self.base.with_suffix('.xcz')); self.upd.unlink()
        app, fam = nsp.scan_nsps(self.tmp)
        fam = nsp.decompress_nsz(self.tmp, app, fam, self.keys, {})
        self.assertEqual(fam['base'].suffix, '.xci')
        self.assertEqual(fam['base'].read_bytes(), b'nsp of Game [v0].xcz')
        self.assertFalse((fam['base'].parent / 'partial').exists())

    def test_failed_conversion_is_not_kept(self):
        app, fam = nsp.scan_nsps(self.tmp); self.fail_conversion = True
        with self.assertRaises(nsp.BuildError): nsp.decompress_nsz(self.tmp, app, fam, self.keys, {})
        out = self.tmp / f'{APP:016X}_build' / 'nsp'
        self.assertEqual(sorted(p.name for p in out.iterdir()), [])   # neither the partial file nor a key
        self.fail_conversion = False
        nsp.decompress_nsz(self.tmp, app, fam, self.keys, {})
        self.assertEqual((out / 'Game [v0].nsp').read_bytes(), b'nsp of Game [v0].nsz')

    def test_an_nsp_beside_wins_and_nsps_need_no_nsz(self):
        pfs0(self.tmp / 'Game [v0].nsp', [f'{APP:016x}0000000000000011.tik']); self.upd.unlink()
        app, fam = nsp.scan_nsps(self.tmp)
        self.assertEqual((fam['base'], fam['updates']), (self.tmp / 'Game [v0].nsp', []))
        with mock.patch.object(importlib.util, 'find_spec', lambda m, *a: None):
            self.assertIs(nsp.decompress_nsz(self.tmp, app, fam, self.tmp / 'prod.keys', {}), fam)

    def test_missing_nsz(self):
        app, fam = nsp.scan_nsps(self.tmp)
        with mock.patch.object(importlib.util, 'find_spec', lambda m, *a: None), mock.patch.object(shutil, 'which', lambda *a, **k: None), self.assertRaisesRegex(nsp.BuildError, 'pip install nsz'):
            nsp.decompress_nsz(self.tmp, app, fam, self.tmp / 'prod.keys', {'PATH': '/nowhere'})

    def standalone(self, help_text):   # no nsz module; a standalone executable on PATH whose --help prints help_text
        done = subprocess.CompletedProcess([], 0, stdout=help_text)
        return (mock.patch.object(importlib.util, 'find_spec', lambda m, *a: None), mock.patch.object(shutil, 'which', lambda n, path=None: '/tools/nsz' if n == 'nsz' else None),
                mock.patch.object(subprocess, 'run', lambda *a, **k: done))

    def test_nsz_executable_on_path(self):   # no nsz module: a standalone nsz 5 executable on PATH converts instead, its keys given by --keys
        app, fam = nsp.scan_nsps(self.tmp); cmds = []
        fake = lambda cmd, log, env=None, cwd=None, on_line=None: (cmds.append(cmd), self.fake_nsz(cmd, log, env, cwd))
        a, b, c = self.standalone('usage: nsz [-D] ... --keys KEYS')
        with a, b, c, mock.patch.object(nsp, 'run', fake):
            fam = nsp.decompress_nsz(self.tmp, app, fam, self.keys, {'PATH': '/tools'})
        self.assertEqual({str(c[0]) for c in cmds}, {'/tools/nsz'})
        self.assertEqual(fam['base'].read_bytes(), b'nsp of Game [v0].nsz')

    def test_nsz_executable_without_keys_option(self):   # an nsz before 5.0 on PATH: it would find no keys, so the build stops and says why
        app, fam = nsp.scan_nsps(self.tmp)
        a, b, c = self.standalone('usage: nsz [-D] ...')
        with a, b, c, self.assertRaisesRegex(nsp.BuildError, 'no --keys option'):
            nsp.decompress_nsz(self.tmp, app, fam, self.keys, {'PATH': '/tools'})

    def test_old_nsz_does_not_hide_the_release_binary(self):
        app, fam = nsp.scan_nsps(self.tmp)
        old, release = '/tools/nsz', '/tools/nsz-cli-macos-arm64'
        with mock.patch.object(importlib.util, 'find_spec', return_value=None), mock.patch.object(shutil, 'which', side_effect=[old, release]), \
             mock.patch.object(subprocess, 'run', side_effect=[subprocess.CompletedProcess([], 0, stdout='usage: nsz [-D]'),
                                                           subprocess.CompletedProcess([], 0, stdout='usage: nsz --keys KEYS')]) as probe:
            nsp.decompress_nsz(self.tmp, app, fam, self.keys, {'PATH': '/tools'})
        self.assertEqual([c.args[0][0] for c in probe.call_args_list], [old, release])
        self.assertEqual(self.runs, [self.base.name, self.upd.name])

    def test_failed_help_does_not_select_an_executable(self):
        old, release = '/tools/nsz', '/tools/nsz-cli-macos-arm64'
        with mock.patch.object(importlib.util, 'find_spec', return_value=None), mock.patch.object(shutil, 'which', side_effect=[old, release]), \
             mock.patch.object(subprocess, 'run', side_effect=[subprocess.CompletedProcess([], 1, stdout='failed to parse --keys'),
                                                           subprocess.CompletedProcess([], 0, stdout='usage: nsz --keys KEYS')]):
            self.assertEqual(nsp.nsz_command({'PATH': '/tools'}), [release])

    def test_relative_path_executable_runs_in_conversion_directory(self):
        previous_cwd = Path.cwd(); self.addCleanup(os.chdir, previous_cwd); os.chdir(self.tmp)
        tools = self.tmp / 'tools'; tools.mkdir()
        exe = tools / 'nsz'
        exe.write_text('#!/bin/sh\nif [ "$1" = "--help" ]; then echo "usage: nsz --keys KEYS"; exit 0; fi\n'
                       '[ "$1" = "--keys" ] && [ -f "$2" ] || exit 1\nshift 2\n[ "$1" = "-D" ] && [ "$2" = "-o" ] || exit 1\n'
                       'out=$3; src=$4; name=${src##*/}; name=${name%.nsz}.nsp\nprintf "converted" > "$out/$name"\n')
        exe.chmod(0o755)
        app, fam = nsp.scan_nsps(self.tmp)
        with mock.patch.object(importlib.util, 'find_spec', return_value=None), mock.patch.object(nsp, 'run', side_effect=lambda cmd, log, **kw: subprocess.run(cmd, check=True, env=kw['env'], cwd=kw['cwd'])):
            fam = nsp.decompress_nsz(self.tmp, app, fam, self.keys, {'PATH': 'tools'})
        self.assertEqual(fam['base'].read_bytes(), b'converted')
        self.assertEqual(fam['updates'][0][1].read_bytes(), b'converted')

if __name__ == '__main__':
    unittest.main()
