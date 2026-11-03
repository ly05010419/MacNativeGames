"""NSP script Mac-only build (scripts/build_ipa_from_nsp.py --platform mac): the shader steps build only the Mac's libraries, each output keeps its own reuse key, a Mac-only build is
rebuilt after an iOS-only NVN build and an iOS build is not reused after a Mac-only one but builds its Mac libraries again (kept for the Mac run command), neither key without the Mac outputs' stamp (inputs.tsv, which the one
chain version's c1_full.tsv / cb_extent.tsv name); the run command's environment file is read as the iOS app reads it.
No game files, keys or Xcode needed (temporary files only in a temporary folder)."""
import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / 'scripts/build_ipa_from_nsp.py'
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', SCRIPT)
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
CHAIN, INPUTS = '0123456789ab', 'aaaaaaaaaaaa'
LIVE, OLD, NEW = 'a' * 64, 'b' * 64, 'c' * 64


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.sh, self.logs, self.ran = self.tmp / 'shader', self.tmp / 'logs', []
        self.cas_used = [LIVE]   # the cache entries a full dmsh_build.py run lists as used
        self.logs.mkdir()
        for cm in (mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', lambda m: None), mock.patch.object(nsp, 'run', self.fake_run)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def fake_run(self, cmd, log, env=None, cwd=None, on_line=None):   # stands in for the translators: writes the libraries a real run writes
        self.ran.append(([str(c) for c in cmd], dict(env or {})))
        tool = Path(cmd[1]).name
        if tool in ('vk_shaders.py', 'gl_shaders.py'):
            api, a = tool[:2], [str(c) for c in cmd]
            outs = [Path(a[a.index('--ios') - 1]), Path(a[a.index('--ios') + 1])] if '--ios' in a else [Path(a[-1])]
            for o in outs: (o / api).mkdir(parents=True, exist_ok=True); (o / api / f'{api}_shaders.metallib').write_bytes(b'lib')
            log.write_text('macosx: 3 shaders\n' + ('iphoneos: 3 shaders\n' if '--ios' in a else ''))
        elif tool == 'own_compilers.py':   # a full dmsh_build.py run: the libraries, their stamp, and the profile's tables naming it
            outs = [self.sh / 'cache'] + ([Path(env['IOSOUT'])] if 'IOSOUT' in env else [])
            targets = (env or {}).get('DMSH_TARGETS', 'mac').split(',')
            for o in outs:
                d = o / f'b-{CHAIN}'; shutil.rmtree(d, ignore_errors=True); d.mkdir(parents=True)
                target = 'mac' if o == self.sh / 'cache' else 'ios'
                if target in targets: (d / 'k_b.metallib').write_bytes(b'lib')
                if target == 'mac': (d / 'k_b.metal').write_text('MSL'); (d / 'k_b.json').write_text('{}'); (d / '_b_cas.txt').write_text(''.join(x + '\n' for x in self.cas_used))
                (o / f'b-{CHAIN}' / 'inputs.tsv').write_text(f'# stamp\ngame\t0100535012974000\ninputs\t{INPUTS}\n')
            for n in ('c1_full.tsv', 'cb_extent.tsv'): (self.tmp / n).write_text(f'# chain b-{CHAIN} inputs {INPUTS} 2026-10-23T00:00:00+00:00\n')
            log.write_text('shaders 1: ok 1\n')


class Layer(Base):
    def vk(self, mac, key='K'):
        self.ran.clear()
        return nsp.layer_shaders(sys.executable, REPO, 'vk', 'Vulkan', [self.tmp / 'romfs'], self.sh, key, self.logs, {'WORK': str(self.tmp)}, mac)

    def test_mac_only(self):   # no --ios, only the Mac key, the Mac folder for DM_MSL_DIR
        self.assertEqual(self.vk(True), self.sh)
        self.assertNotIn('--ios', self.ran[0][0])
        self.assertTrue((self.sh / 'vk.key').is_file()); self.assertFalse((self.sh / 'ios/vk.key').exists())
        self.assertEqual(self.vk(True), self.sh); self.assertEqual(self.ran, [])   # reused

    def test_mac_after_ios(self):   # an iOS build makes the Mac library too: a Mac-only build is reused
        self.assertEqual(self.vk(False), self.sh / 'ios')
        self.assertIn('--ios', self.ran[0][0])
        (self.sh / 'vk.key').unlink()   # an iOS build from before Mac-only builds wrote only its own key
        self.assertEqual(self.vk(True), self.sh); self.assertEqual(self.ran, [])

    def test_ios_after_mac(self):   # a Mac-only build made no iPhone library: an iOS build runs
        self.vk(True)
        self.assertEqual(self.vk(False), self.sh / 'ios'); self.assertEqual(len(self.ran), 1)
        self.assertTrue((self.sh / 'ios/vk/vk_shaders.metallib').is_file())

    def test_changed_inputs(self):
        self.vk(False)
        self.vk(True, 'K2'); self.assertEqual(len(self.ran), 1)
        self.assertTrue((self.sh / 'ios/vk/vk_shaders.metallib').is_file())   # the iPhone library stays, under its own (older) key
        self.assertEqual(nsp.reused(self.sh / 'ios/vk.key', 'K'), '3 shaders for the Mac and iOS')


class Nvn(Base):
    def setUp(self):
        super().setUp()
        self.g = types.SimpleNamespace(table=lambda n: self.tmp / n, dir=self.tmp)
        for cm in (mock.patch.object(nsp, 'fingerprint', lambda *a, **k: 'K'), mock.patch.object(nsp, 'chain_of', lambda repo: CHAIN)):   # one chain version for every game
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def build(self, mac):
        self.ran.clear()
        return nsp.build_shaders(sys.executable, REPO, self.sh, REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate', {'WORK': str(self.tmp), 'IOSOUT': '/elsewhere'}, self.logs, self.g, pkg_key='P', mac=mac)

    def test_mac_only(self):   # no IOSOUT (not even the caller's), the Mac key beside the cache folder, not in it
        self.assertEqual(self.build(True), self.sh / 'cache')
        self.assertNotIn('IOSOUT', self.ran[0][1])
        self.assertTrue((self.sh / f'mac-b-{CHAIN}.key').is_file()); self.assertEqual([p.name for p in (self.sh / 'cache').iterdir()], [f'b-{CHAIN}'])
        self.assertEqual(self.build(True), self.sh / 'cache'); self.assertEqual(self.ran, [])

    def test_mac_after_ios(self):
        self.assertEqual(self.build(False), self.sh / 'ios')
        self.assertEqual(self.ran[0][1]['IOSOUT'], str(self.sh / 'ios'))
        self.assertEqual(self.ran[0][1]['DMSH_TARGETS'], 'ios')
        self.assertFalse((self.sh / f'mac-b-{CHAIN}.key').exists())
        self.assertFalse(list((self.sh / 'cache').glob('b-*/*.metallib')))
        self.assertEqual(self.build(True), self.sh / 'cache'); self.assertEqual(len(self.ran), 1)
        self.assertEqual(self.ran[0][1]['DMSH_TARGETS'], 'mac')
        self.assertEqual(self.build(True), self.sh / 'cache'); self.assertEqual(self.ran, [])

    def test_ios_repeat_reuses_its_target(self):
        self.build(False)
        self.assertEqual(self.build(False), self.sh / 'ios'); self.assertEqual(self.ran, [])

    def test_ios_after_mac_keeps_mac_libraries(self):   # the Mac run command a Mac-only build printed still finds its libraries after an iOS build
        self.build(True)
        self.assertEqual(self.build(False), self.sh / 'ios'); self.assertEqual(len(self.ran), 1)
        self.assertEqual(self.ran[0][1]['DMSH_TARGETS'], 'mac,ios')
        self.assertTrue((self.sh / 'cache' / f'b-{CHAIN}' / 'k_b.metallib').is_file()); self.assertTrue((self.sh / 'ios' / f'b-{CHAIN}' / 'k_b.metallib').is_file())
        self.assertEqual(self.build(True), self.sh / 'cache'); self.assertEqual(self.ran, [])   # both keys written: the Mac build is reused
        self.assertEqual(self.build(False), self.sh / 'ios'); self.assertEqual(self.ran, [])

    def test_ios_after_mac_of_another_chain(self):   # libraries of another chain version are not this build's to keep
        self.build(True)
        (self.sh / 'cache' / f'b-{CHAIN}').rename(self.sh / 'cache' / 'b-ba9876543210')
        self.build(False); self.assertEqual(self.ran[0][1]['DMSH_TARGETS'], 'ios')

    def stale(self):   # two older chain versions' libraries and keys, in both library folders
        for old in ('157e1b96c8da', 'a78f966a7b38'):
            for d in (self.sh / 'cache', self.sh / 'ios'):
                (d / f'b-{old}').mkdir(parents=True); (d / f'b-{old}' / 'k_b.metallib').write_bytes(b'x' * 100)
            (self.sh / 'ios' / f'b-{old}.key').write_text('K\n'); (self.sh / f'mac-b-{old}.key').write_text('K\n')

    def left(self):
        return sorted(str(p.relative_to(self.sh)) for p in self.sh.rglob('*') if p.parent.name in ('cache', 'ios', 'shader') and (p.name.startswith(('b-', 'mac-b-'))))

    def test_prune_stale_chains_after_success(self):   # the old chain versions go with their keys, the current one stays, the size is logged once
        self.stale()
        said = []
        with mock.patch.object(nsp, 'say', said.append):
            self.assertEqual(self.build(False), self.sh / 'ios')
        self.assertEqual(self.left(), [f'cache/b-{CHAIN}', f'ios/b-{CHAIN}', f'ios/b-{CHAIN}.key'])
        self.assertTrue((self.sh / 'ios' / f'b-{CHAIN}' / 'k_b.metallib').is_file())
        self.assertEqual([m for m in said if m.startswith('removed old shader libraries')], ['removed old shader libraries b-157e1b96c8da, b-a78f966a7b38 (0.4 KB)'])
        self.stale(); said.clear()   # a reused build prunes too
        with mock.patch.object(nsp, 'say', said.append):
            self.build(False)
        self.assertEqual(self.ran, []); self.assertEqual(len(self.left()), 3)

    def test_failed_build_keeps_stale_chains(self):   # nothing goes before the new libraries exist
        self.stale(); before = self.left()
        def failing(cmd, log, env=None, cwd=None, on_line=None): raise nsp.BuildError('translator failed')
        with mock.patch.object(nsp, 'run', failing), self.assertRaises(nsp.BuildError): self.build(False)
        self.assertEqual(self.left(), before)
        with mock.patch.object(nsp, 'run', lambda *a, **k: None), self.assertRaises(nsp.BuildError): self.build(False)   # no libraries written
        self.assertEqual(self.left(), before)

    def test_failed_prune_keeps_the_build(self):   # a folder that cannot be removed is a [RED] line; the others go and the build still returns its libraries
        self.stale()
        real, said = nsp.rmtree, []
        def stuck(p):
            if p.name == 'b-157e1b96c8da' and p.parent.name == 'ios': raise PermissionError(13, 'Permission denied', str(p))
            real(p)
        with mock.patch.object(nsp, 'rmtree', stuck), mock.patch.object(nsp, 'say', said.append):
            self.assertEqual(self.build(False), self.sh / 'ios')
        self.assertEqual(self.left(), sorted([f'cache/b-{CHAIN}', 'ios/b-157e1b96c8da', f'ios/b-{CHAIN}', f'ios/b-{CHAIN}.key']))
        self.assertEqual(len([m for m in said if m.startswith('[RED] could not remove old shader libraries')]), 1)
        self.assertEqual(len([m for m in said if m.startswith('removed old shader libraries b-157e1b96c8da, b-a78f966a7b38 (')]), 1)

    def test_prune_ignores_other_names(self):   # only b-<12 hex> folders and their keys, never the compile cache or another folder
        self.build(True)
        for n in ('cas', 'b-notahexchain', 'b-0123456789abcd', 'mine'): (self.sh / 'cache' / n).mkdir()
        (self.sh / 'cache' / 'b-ffffffffffff').write_text('a file, not a library folder')
        self.build(False)
        self.assertEqual(sorted(p.name for p in (self.sh / 'cache').iterdir()), sorted(['b-0123456789abcd', 'b-ffffffffffff', 'b-notahexchain', f'b-{CHAIN}', 'cas', 'mine']))

    def cas(self, *names, when=0):   # cache entries with a source and a library (a clone of the compile cache's), last changed at when
        for n in names:
            d = self.sh / 'cache/cas/shader' / n / 'mac'; d.mkdir(parents=True)
            (d / 'k_b.metal').write_bytes(b'm' * 100); (d / 'k_b.metallib').write_bytes(b'l' * 1000)
            nsp.os.utime(d.parent, (when, when))

    def test_prune_shader_cache(self):   # entries the build did not list go once it succeeded; a listed one, one newer than the list and other names stay; only the sources count
        self.cas(LIVE, OLD, 'notadigest')
        said = []
        with mock.patch.object(nsp, 'say', said.append): self.build(False)
        self.cas(NEW, when=nsp.time.time() + 3600)   # published by another build sharing the cache after this one's list
        self.assertEqual(sorted(p.name for p in (self.sh / 'cache/cas/shader').iterdir()), sorted([LIVE, NEW, 'notadigest']))
        self.assertEqual([m for m in said if 'shader cache entries' in m], ['removed old shader cache entries: 1 (0.1 KB)'])
        self.cas(OLD); said.clear()   # a reused build prunes too
        with mock.patch.object(nsp, 'say', said.append): self.build(False)
        self.assertEqual(self.ran, []); self.assertFalse((self.sh / 'cache/cas/shader' / OLD).exists()); self.assertTrue((self.sh / 'cache/cas/shader' / NEW).exists())

    def test_prune_shader_cache_needs_success_and_the_list(self):   # a failed build, or libraries from before the list, keep every entry
        self.build(False); self.cas(OLD)
        (self.sh / 'ios' / f'b-{CHAIN}.key').unlink()   # not reused: the step runs, and fails
        def failing(cmd, log, env=None, cwd=None, on_line=None): raise nsp.BuildError('translator failed')
        with mock.patch.object(nsp, 'run', failing), self.assertRaises(nsp.BuildError): self.build(False)
        self.assertTrue((self.sh / 'cache/cas/shader' / OLD).exists())
        self.build(False); self.assertFalse((self.sh / 'cache/cas/shader' / OLD).exists())
        (self.sh / 'cache' / f'b-{CHAIN}' / '_b_cas.txt').unlink(); self.cas(OLD)
        self.build(False)
        self.assertEqual(self.ran, []); self.assertTrue((self.sh / 'cache/cas/shader' / OLD).exists())

    def test_no_stamp(self):   # Mac libraries no full build stamped (an interrupted one): no table can name them, so neither key reuses them
        self.build(False)
        (self.sh / 'cache' / f'b-{CHAIN}' / 'inputs.tsv').unlink()
        self.assertEqual(self.build(True), self.sh / 'cache'); self.assertEqual(len(self.ran), 1)
        self.assertTrue((self.sh / 'cache' / f'b-{CHAIN}' / 'inputs.tsv').is_file())

    def test_stale_tables(self):   # a reused build makes a table again when it does not name the Mac libraries' stamp
        self.build(True)
        (self.tmp / 'cb_extent.tsv').write_text(f'# chain b-{CHAIN} inputs bbbbbbbbbbbb 2026-10-23T00:00:00+00:00\n')
        self.assertEqual(self.build(True), self.sh / 'cache'); self.assertEqual([Path(c[1]).name for c, _ in self.ran], ['cb_extent.py'])

    def test_split_pairs_invalidate_shader_reuse(self):
        # A registered profile may have no pso_pairs.tsv: the split's copy feeds vs_pad.
        def fingerprint(*args, **kwargs):
            path = next(x[1][0] for x in args if isinstance(x, tuple) and x[0] == 'files' and x[1] and x[1][0].name == 'pso_pairs.tsv')
            return str(path.read_bytes() if path.is_file() else b'missing')
        with mock.patch.object(nsp, 'fingerprint', side_effect=fingerprint):
            self.build(True); self.assertEqual(len(self.ran), 1)
            self.build(True); self.assertEqual(self.ran, [])
            (self.sh / 'pso_pairs.tsv').write_text('v\tf1\n')
            self.build(True); self.assertEqual(len(self.ran), 1)
            (self.sh / 'pso_pairs.tsv').write_text('v\tf2\n')
            self.build(True); self.assertEqual(len(self.ran), 1)
            self.build(True); self.assertEqual(self.ran, [])

    def test_shader_watch_finishes_extra_pairs_last(self):
        (self.tmp / 'shader_map.tsv').write_text('p.bin\t0\tvs\ta\tsrc\np.bin\t1\tps\tb\tsrc\np.bin\t2\tvs\tc\tsrc\n')
        (self.tmp / 'shader_pairs.tsv').write_text('c\tb\n')
        pso = self.tmp / 'pso_pairs.tsv'; pso.write_text('a\tb\n')
        watch = nsp.ShaderWatch(self.g, self.tmp, self.tmp, pso)
        self.assertEqual(list(watch.pos), ['b', 'a', 'c'])


class EnvFile(Base):
    def test_pairs(self):
        p = self.tmp / 'env.txt'; p.write_text('# a comment\nDM_A=1\n  DM_B = x=y\n=nokey\nnoeq\n\nDM_C=\n')
        self.assertEqual(nsp.env_file_pairs(p), {'DM_A': '1', 'DM_B': ' x=y', 'DM_C': ''})

    def test_missing(self):
        with self.assertRaises(nsp.BuildError): nsp.env_file_pairs(self.tmp / 'none.txt')


if __name__ == '__main__':
    unittest.main()
