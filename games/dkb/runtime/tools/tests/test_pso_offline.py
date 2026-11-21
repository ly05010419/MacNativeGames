"""The pipeline archive made offline (dm/gfx/tools/pso_offline.py): the pairs as the host reads them, the stamp the host compares (pso_prebuild's pre_stamp), the function list of a
.metallib header, the bases the host's prebuild would make (pre_desc) and those it leaves out, the pipelines script, and the per-architecture reuse of metal-tt's archives.
No game files, Xcode or GPU needed (metal-tt is replaced; temporary files only in a temporary folder)."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import types
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import tracemalloc
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('pso_offline', REPO / 'dm/gfx/tools/pso_offline.py')
po = importlib.util.module_from_spec(spec); spec.loader.exec_module(po)
CHAIN = '0123456789ab'


def tag(name, data): return name + struct.pack('<H', len(data)) + data


def metallib(funcs, layer=False):
    """A .metallib header as the Metal compiler writes it: the function list (count, then per function its size counting the size field, NAME, ENDT) and the public metadata
    (per function its size not counting the size field, a CNST tag for a function with constants, ENDT); funcs: [(name, has constants)]"""
    fl = struct.pack('<I', len(funcs))
    for name, _ in funcs:
        body = tag(b'NAME', name.encode() + b'\0') + tag(b'TYPE', b'\0') + b'ENDT'
        fl += struct.pack('<I', 4 + len(body)) + body
    pm = b''
    for _, const in funcs:
        body = (tag(b'CNST', b'\x01\x00spvLinearTextureAlignmentOverride\x00\x35\xff\xff\x00') if const else b'') + b'ENDT'
        pm += struct.pack('<I', len(body)) + body
    fo = 0x58; po_ = fo + len(fl)
    head = b'MTLB' + b'\0' * 0x14 + struct.pack('<QQQQ', fo, len(fl), po_, len(pm))
    head += b'\0' * (fo - len(head))
    return head + fl + pm + (b'render_target_array_index' if layer else b'') + b'BC\xc0\xde'


class Pairs(unittest.TestCase):
    def test_read_pairs_as_the_host(self):
        text = b'# header\nv1\tf1\n\nv2\tf2\nv1\tf1\nv3\n v4\tf4\nv1\tf1\textra\n'
        self.assertEqual(po.read_pairs(text), [('v1', 'f1'), ('v2', 'f2'), (' v4', 'f4'), ('v1', 'f1')])   # the last: another line, the same pair (plan drops the base twice)

    def test_stamp_matches_the_hosts_format(self):
        text = b'a\tb\n' + b'c\td\n'
        self.assertEqual(po.stamp(CHAIN, False, text), f'chain\t{CHAIN}\ndepth\tnear=-w\npairs\t8\t{hashlib.sha256(text).hexdigest()[:16]}\n')
        self.assertIn('depth\tnear=0\n', po.stamp(CHAIN, True, text))

    def test_pair_text_joins_the_lists_and_skips_a_missing_one(self):
        with tempfile.TemporaryDirectory() as t:
            a, b = Path(t, 'shader_pairs.tsv'), Path(t, 'pso_pairs.tsv')
            a.write_bytes(b'v\tf\n'); self.assertEqual(po.pair_text([a, b]), b'v\tf\n')
            b.write_bytes(b'w\tg\n'); self.assertEqual(po.pair_text([a, b]), b'v\tf\nw\tg\n')

    def test_arch_names(self):
        self.assertEqual(po.arch_name('g18p'), 'applegpu_g18p')
        self.assertEqual(po.arch_name(' applegpu_g17p'), 'applegpu_g17p')


class Functions(unittest.TestCase):
    def test_function_list_and_constants(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t, 'x.metallib'); p.write_bytes(metallib([('main0', False), ('main0_z', False), ('fmain', True)]))
            self.assertEqual(po.functions(p), {'main0': False, 'main0_z': False, 'fmain': True})
            p.write_bytes(b'not a library'); self.assertRaises(ValueError, po.functions, p)
            p.write_bytes(metallib([('main0', False)])[:0x60]); self.assertRaises(ValueError, po.functions, p)

    @unittest.skipUnless(shutil.which('xcrun'), 'needs the Metal compiler')
    def test_a_real_library(self):
        src = ('#include <metal_stdlib>\nusing namespace metal;\nconstant bool k [[function_constant(0)]];\nstruct V { float4 p [[position]]; };\n'
               'vertex V vs(uint i [[vertex_id]]) { V o; o.p = float4(i); return o; }\nfragment float4 fs() { return is_function_constant_defined(k) ? 1 : 0; }\n')
        with tempfile.TemporaryDirectory() as t:
            m, air, lib = Path(t, 'a.metal'), Path(t, 'a.air'), Path(t, 'a.metallib'); m.write_text(src)
            r = subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metal', '-c', m, '-o', air], capture_output=True)
            if r.returncode: self.skipTest('no iOS Metal compiler')
            subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metallib', air, '-o', lib], check=True, capture_output=True)
            self.assertEqual(po.functions(lib), {'vs': False, 'fs': True})


class Plan(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp()) / f'b-{CHAIN}'; self.d.mkdir(); self.addCleanup(shutil.rmtree, self.d.parent)

    def shader(self, key, j, libs):   # reflection <key>_b.json and libraries {suffix: funcs or (funcs, layer)}
        (self.d / f'{key}_b.json').write_text(json.dumps(dict({'chain': CHAIN, 'entry': 'main0'}, **j)))
        for suf, f in libs.items():
            funcs, layer = f if isinstance(f, tuple) else (f, False)
            (self.d / f'{key}{suf}.metallib').write_bytes(metallib(funcs, layer))

    def test_bases_rest_and_left_out(self):
        vz = [('main0', False), ('main0_z', False)]
        self.shader('pull', {'entry_z': 'main0_z', 'pull': {'entry': 'main0'}, 'vertex_inputs': [{'location': 0}]}, {'_b': vz, '_bp': vz})
        self.shader('plain', {'entry_z': 'main0_z', 'vertex_inputs': []}, {'_b': vz})
        self.shader('inputs', {'vertex_inputs': [{'location': 0}]}, {'_b': vz})
        self.shader('layer', {'pull': {}}, {'_b': vz, '_bp': (vz, True)})
        self.shader('other', {'chain': 'fedcba987654'}, {'_b': vz})
        self.shader('nolib', {'pull': {}}, {'_b': vz})   # no _bp
        self.shader('fs', {}, {'_b': [('main0', False)]})
        self.shader('fsc', {}, {'_b': [('main0', True)]})
        self.shader('fsx', {'entry': 'main1'}, {'_b': [('main0', False)]})
        self.shader('fsp', {'raster': {'primitive_id': 1}}, {'_b': [('main0', False)]})   # reads the primitive ID: no base
        pairs = [('pull', 'fs'), ('plain', 'fs'), ('inputs', 'fs'), ('layer', 'fs'), ('other', 'fs'), ('nolib', 'fs'), ('pull', 'fsc'), ('pull', 'fsx'), ('pull', 'gone'), ('pull', 'fs'),
                 ('plain', 'fsp')]
        bases, rest, left = po.plan(self.d, CHAIN, pairs, False)
        self.assertEqual(bases, [('pull_bp', 'main0', 'fs_b', 'main0'), ('plain_b', 'main0', 'fs_b', 'main0')])
        self.assertEqual(rest, [('pull', 'fsc')])
        self.assertEqual(left, {'reflection': 2, 'stage inputs': 1, 'library': 1, 'layer': 1, 'primitive ID': 1, 'entry': 1})
        bases, _, _ = po.plan(self.d, CHAIN, pairs[:2], True)   # near = 0: the entry_z entries
        self.assertEqual(bases, [('pull_bp', 'main0_z', 'fs_b', 'main0'), ('plain_b', 'main0_z', 'fs_b', 'main0')])

    def test_script(self):
        j = po.script(self.d, [('v_bp', 'main0', 'f_b', 'main0'), ('w_bp', 'main0_z', 'f_b', 'main0')])
        self.assertEqual([l['label'] for l in j['libraries']], ['v_bp', 'f_b', 'w_bp'])
        self.assertEqual(j['libraries'][0]['path'], str(self.d.resolve() / 'v_bp.metallib'))
        fns = j['function_descriptors']['library_function_descriptors']
        self.assertEqual([(f['label'], f['library'], f['name']) for f in fns],
                         [('v_bp.main0', 'lib:v_bp', 'main0'), ('f_b.main0', 'lib:f_b', 'main0'), ('w_bp.main0_z', 'lib:w_bp', 'main0_z')])
        p = j['pipeline_descriptors']['render_pipeline_descriptors']
        self.assertEqual([(x['vertex_function_descriptor'], x['fragment_function_descriptor']) for x in p],
                         [('fnd:v_bp.main0', 'fnd:f_b.main0'), ('fnd:w_bp.main0_z', 'fnd:f_b.main0')])
        self.assertEqual(set(p[0]), {'vertex_function_descriptor', 'fragment_function_descriptor', 'color_attachments'})   # no vertex descriptor, one sample: the defaults
        self.assertEqual(len(p[0]['color_attachments']), 8)
        self.assertTrue(all(v == 'Unspecialized' for a in p[0]['color_attachments'] for v in a.values()))
        self.assertEqual(len(p[0]['color_attachments'][0]), 9)


class Build(unittest.TestCase):
    """main: one archive per architecture, made again only when its key changes; the stamp, rest_pairs.tsv and archive.tsv beside them"""
    def setUp(self):
        self.t = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.t)
        self.lib = self.t / f'b-{CHAIN}'; self.lib.mkdir()
        for k, j, funcs in (('v', {'pull': {}, 'entry_z': 'main0_z'}, [('main0', False), ('main0_z', False)]), ('f', {}, [('main0', False)]), ('c', {}, [('main0', True)])):
            (self.lib / f'{k}_b.json').write_text(json.dumps(dict({'chain': CHAIN, 'entry': 'main0'}, **j)))
            (self.lib / f'{k}_b.metallib').write_bytes(metallib(funcs))
        (self.lib / 'v_bp.metallib').write_bytes(metallib([('main0', False), ('main0_z', False)]))
        (self.lib / 'inputs.tsv').write_text('game\tX\ninputs\tY\n')
        self.prof = self.t / 'games/0100000000001000'; self.prof.mkdir(parents=True)
        (self.prof / 'game.tsv').write_text('title_id\t0100000000001000\nname\tTest\ndepth_mode\tzero\n')
        (self.prof / 'shader_pairs.tsv').write_text('# pairs\nv\tf\nv\tc\n')
        self.out, self.work, self.calls = self.t / 'out', self.t / 'work', []
        self.compiler = 'AIR-TT 1.0'

    def fake_run(self, cmd, **kw):
        cmd = [str(c) for c in cmd]
        if '-version' in cmd: return subprocess.CompletedProcess(cmd, 0, self.compiler + '\n', '')
        if '--show-sdk-version' in cmd: return subprocess.CompletedProcess(cmd, 0, '27.1\n', '')
        self.calls.append(cmd); Path(cmd[cmd.index('-o') + 1]).write_bytes(b'archive ' + cmd[cmd.index('-arch') + 1].encode())
        return subprocess.CompletedProcess(cmd, 0)

    def main(self, *extra):
        args = ['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work), *extra]
        with mock.patch.object(po.subprocess, 'run', self.fake_run), mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(io.StringIO()):
            return po.main(args)

    def test_large_script_memory_and_compatible_archive_key(self):
        # Share the two synthetic library files but give every base a distinct entry pair.
        bases = [('v_bp', f'vertex{i}', 'f_b', 'main0') for i in range(512)]
        at_compile = []
        compiler = self.fake_run
        def measure(cmd, **kwargs):
            if '-o' in cmd:
                at_compile.append(tracemalloc.get_traced_memory()[0])
            return compiler(cmd, **kwargs)
        with mock.patch.object(po, 'plan', return_value=(bases, [], {})), mock.patch.object(self, 'fake_run', measure):
            tracemalloc.start()
            try:
                self.main('--os-min', '27.0')
                peak = tracemalloc.get_traced_memory()[1]
            finally:
                tracemalloc.stop()
        path = self.work / 'bases.mtl4-json'
        self.assertFalse((self.work / 'bases.mtl4-json.partial').exists())
        self.assertLess(peak, 3 * path.stat().st_size, 'serializing descriptors must not retain multiple full JSON buffers')
        self.assertEqual(len(at_compile), 1)
        self.assertLess(at_compile[0], path.stat().st_size, 'the compiler only needs the script on disk')
        config = json.loads(path.read_bytes())
        self.assertEqual(len(config['pipeline_descriptors']['render_pipeline_descriptors']), len(bases))
        # Keys from earlier builds must continue to hit. Only library locations are normalized.
        for lib in config['libraries']:
            lib['path'] = lib['label'] + '.metallib'
        inputs = dict(format=1, configuration=config, libraries=po.library_digest(self.lib, bases),
                      arch='applegpu_g18p', platform='ios', os_min='27.0', toolchain=self.compiler)
        expected = hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        self.assertEqual((self.work / 'key.applegpu_g18p').read_text().strip(), expected)

    def test_build_reuse_and_stamp(self):
        self.assertEqual(self.main('--gpu', 'g18p'), 0)
        self.assertEqual(len(self.calls), 1)
        c = self.calls[0]
        self.assertEqual(c[:8], ['xcrun', '-sdk', 'iphoneos', 'metal-tt', '-arch', 'applegpu_g18p', '-ios-version-min', '27.0'])
        self.assertTrue(c[8].endswith('.mtl4-json'))   # the extension metal-tt takes the Metal 4 format from
        script = json.loads(Path(c[8]).read_text())
        self.assertEqual(script['pipeline_descriptors']['render_pipeline_descriptors'][0]['vertex_function_descriptor'], 'fnd:v_bp.main0_z')   # the profile's depth_mode zero
        self.assertEqual((self.out / 'applegpu_g18p.mtl4a').read_bytes(), b'archive applegpu_g18p')
        text = (self.prof / 'shader_pairs.tsv').read_bytes()
        self.assertEqual((self.out / 'prebuilt.tsv').read_text(), po.stamp(CHAIN, True, text))
        self.assertEqual((self.out / 'rest_pairs.tsv').read_text(), 'v\tc\n')
        self.assertIn('bases\t1\n', (self.out / 'archive.tsv').read_text())
        self.assertEqual(self.main('--gpu', 'g18p'), 0); self.assertEqual(len(self.calls), 1)   # unchanged: reused
        self.assertEqual(self.main('--gpu', 'g18p,applegpu_g17p'), 0)   # another architecture: only it is built
        self.assertEqual([x[5] for x in self.calls[1:]], ['applegpu_g17p'])
        self.assertEqual(self.main('--gpu', 'g17p', '--os-min', '26.0'), 0)   # another minimum: built again; g18p no longer asked for: removed
        self.assertEqual(self.calls[-1][7], '26.0')
        self.assertEqual(sorted(p.name for p in self.out.glob('*.mtl4a')), ['applegpu_g17p.mtl4a'])
        (self.prof / 'shader_pairs.tsv').write_text('v\tf\n')   # only a device-specialized pair removed: offline bases unchanged
        self.assertEqual(self.main('--gpu', 'g17p', '--os-min', '26.0'), 0)
        self.assertEqual(len(self.calls), 3)
        self.assertFalse((self.out / 'rest_pairs.tsv').exists())

    def test_chain_move_reuses_binary_and_refreshes_metadata_for_both_platforms(self):
        for platform in ('macos', 'ios'):
            with self.subTest(platform=platform):
                self.out, self.work = self.t / f'out-{platform}', self.t / f'work-{platform}'
                args = ('--platform', platform, '--os-min', '27.0')
                self.main(*args)
                archive = self.out / 'applegpu_g18p.mtl4a'
                identity = archive.stat().st_ino, archive.stat().st_mtime_ns, archive.read_bytes()
                before = len(self.calls)
                chain = 'fedcba987654' if platform == 'macos' else 'abcdef012345'
                moved = self.t / platform / f'b-{chain}'
                shutil.copytree(self.lib, moved)
                for p in moved.glob('*_b.json'):
                    j = json.loads(p.read_text()); j['chain'] = chain; p.write_text(json.dumps(j))
                (moved / 'inputs.tsv').write_text('game\tX\ninputs\tupdated-profile\n')
                self.lib = moved
                self.assertEqual(self.main(*args), 0)
                self.assertEqual(len(self.calls), before, 'chain-only move must not invoke metal-tt')
                self.assertEqual((archive.stat().st_ino, archive.stat().st_mtime_ns, archive.read_bytes()), identity)
                self.assertEqual((self.out / 'prebuilt.tsv').read_text(), po.stamp(chain, True, (self.prof / 'shader_pairs.tsv').read_bytes()))
                self.assertIn(str(moved), (self.work / 'bases.mtl4-json').read_text())

    def test_metadata_only_changes_reuse_but_refresh_runtime_lists(self):
        self.main()
        (self.lib / 'inputs.tsv').write_text('game\tX\ninputs\tnew-profile\n')
        pairs = b'# changed comment\nv\tf\nv\tf\n'
        (self.prof / 'shader_pairs.tsv').write_bytes(pairs)
        self.main()
        self.assertEqual(len(self.calls), 1)
        self.assertFalse((self.out / 'rest_pairs.tsv').exists())
        self.assertEqual((self.out / 'prebuilt.tsv').read_text(), po.stamp(CHAIN, True, pairs))

    def test_pipeline_entry_and_descriptor_changes_rebuild(self):
        self.main()
        self.main('--depth', 'gl')
        self.assertEqual(len(self.calls), 2)
        original = po.script
        def changed(lib_dir, bases):
            config = original(lib_dir, bases)
            config['pipeline_descriptors']['render_pipeline_descriptors'][0]['raster_sample_count'] = 4
            return config
        with mock.patch.object(po, 'script', changed):
            self.main('--depth', 'gl')
            self.assertEqual(len(self.calls), 3)
            self.main('--depth', 'gl')
            self.assertEqual(len(self.calls), 3)

    def test_platform_minimum_and_compiler_changes_rebuild(self):
        self.main('--platform', 'ios', '--os-min', '27.0')
        self.main('--platform', 'macos', '--os-min', '27.0')
        self.assertEqual(len(self.calls), 2)
        self.main('--platform', 'macos', '--os-min', '26.0')
        self.assertEqual(len(self.calls), 3)
        self.compiler = 'AIR-TT 2.0'
        self.main('--platform', 'macos', '--os-min', '26.0')
        self.assertEqual(len(self.calls), 4)

    def test_missing_archive_and_legacy_key_rebuild(self):
        self.main()
        (self.out / 'applegpu_g18p.mtl4a').unlink()
        self.main()
        self.assertEqual(len(self.calls), 2)
        (self.work / 'key.applegpu_g18p').write_text('legacy-cache-key\n')
        self.main()
        self.assertEqual(len(self.calls), 3)

    def test_stale_reflection_cannot_reuse_archive(self):
        self.main()
        p = self.lib / 'v_b.json'
        j = json.loads(p.read_text()); j['chain'] = 'stale'; p.write_text(json.dumps(j))
        with self.assertRaises(SystemExit): self.main()
        self.assertEqual(len(self.calls), 1)

    def test_macos_takes_this_macs_major_version(self):   # not the SDK's: Xcode 27.1's SDK is 27.0 on macOS 26.6, which cannot open a 27.0 archive (GitHub issue #673)
        def run(cmd, **kw):
            cmd = [str(c) for c in cmd]
            if cmd == ['sw_vers', '-productVersion']: return subprocess.CompletedProcess(cmd, 0, '26.6.2\n', '')
            return self.fake_run(cmd)
        with mock.patch.object(po.subprocess, 'run', run), mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(po.main(['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work), '--platform', 'macos', '--gpu', 'g16s']), 0)
        self.assertEqual(self.calls[0][:8], ['xcrun', '-sdk', 'macosx', 'metal-tt', '-arch', 'applegpu_g16s', '-macos-version-min', '26.0'])

    def test_depth_override_and_bad_key(self):
        self.main('--depth', 'gl', '--script-only')
        self.assertIn('depth\tnear=-w\n', (self.out / 'prebuilt.tsv').read_text()); self.assertEqual(self.calls, [])
        (self.prof / 'game.tsv').write_text('title_id\t0100000000001000\nname\tTest\ndepth_mode\tnear0\n')
        with self.assertRaises(SystemExit): self.main('--script-only')

    def test_profile_archives_match_observed_depth(self):
        # Issue #758 and its seven-game follow-up, then four found by reading the NSP script's check boots: these builds' NVN calls select near=0 (evidence in each game note).
        # Exercise the real profiles through the builder with synthetic pairs: an omitted key must not quietly build main0.
        profiles = ('01008CF01BAAC000', '010089C01E9CA000', '010056B015FE8000', '01007BB017812000',
                    '01003E601E324000', '010020400E1C2000', '0100429011144000', '010014C011146000',
                    '0100ACD0163D0000', '010040801A4C8000', '0100C49025D3E000', '0100A3900C3E2000')
        for gid in profiles:
            with self.subTest(game=gid):
                self.main('--game', gid, '--pairs', str(self.prof / 'shader_pairs.tsv'), '--script-only')
                self.assertIn('depth\tnear=0\n', (self.out / 'prebuilt.tsv').read_text())
                script = json.loads(next(self.work.glob('*.mtl4-json')).read_text())
                self.assertEqual(script['pipeline_descriptors']['render_pipeline_descriptors'][0]['vertex_function_descriptor'], 'fnd:v_bp.main0_z')

    def test_library_content_invalidates_archive(self):
        self.main()
        for name in ('v_bp', 'f_b'):
            with self.subTest(library=name):
                lib = self.lib / f'{name}.metallib'
                lib.write_bytes(lib.read_bytes() + b'changed GPU program')   # same entries, reflection and profile inputs
                before = len(self.calls)
                self.main()
                self.assertEqual(len(self.calls), before + 1)
                self.main()
                self.assertEqual(len(self.calls), before + 1)   # unchanged bytes still reuse the archive

    def test_constant_only_pairs_are_left_to_device(self):
        self.main()   # remove the previous archive when no pair can be built offline anymore
        (self.prof / 'shader_pairs.tsv').write_text('v\tc\n')
        before = len(self.calls)
        self.assertEqual(self.main(), 0)
        self.assertEqual(len(self.calls), before)
        self.assertEqual(list(self.out.glob('*.mtl4a')), [])
        self.assertEqual((self.out / 'rest_pairs.tsv').read_text(), 'v\tc\n')
        self.assertEqual((self.out / 'prebuilt.tsv').read_text(), po.stamp(CHAIN, True, b'v\tc\n'))
        self.assertIn('bases\t0\n', (self.out / 'archive.tsv').read_text())

    def test_no_pairs_builds_nothing(self):   # GitHub issue #338: a header-only shader_pairs.tsv is nothing to build, not an error
        self.main()
        (self.prof / 'shader_pairs.tsv').write_text('# pairs\n')
        before = len(self.calls)
        self.assertEqual(self.main(), 0)
        self.assertEqual(len(self.calls), before)
        self.assertEqual(list(self.out.glob('*.mtl4a')), [])   # the old archive goes: mac_pso() then packs the app without one
        self.assertFalse((self.out / 'rest_pairs.tsv').exists())
        self.assertIn('bases\t0\n', (self.out / 'archive.tsv').read_text())

    def test_pairs_all_left_out_is_an_error(self):   # pairs whose libraries the host would not take: still a failure
        (self.prof / 'shader_pairs.tsv').write_text('x\ty\n')
        with self.assertRaises(SystemExit): self.main()

    def test_metal_tt_failure_leaves_no_archive(self):
        def fail(cmd, **kw):
            cmd = [str(c) for c in cmd]
            if '-version' in cmd or '--show-sdk-version' in cmd: return self.fake_run(cmd)
            return subprocess.CompletedProcess(cmd, 1)
        with mock.patch.object(po.subprocess, 'run', fail), mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
            po.main(['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work)])
        self.assertEqual(list(self.out.glob('*.mtl4a')), [])

    def test_low_disk_space_stops_before_metal_tt(self):   # Cruis'n Blast: a full volume made metal-tt fail with only "applegpu-nt command failed"
        self.work.mkdir(parents=True); (self.work / 'translate-0001').write_bytes(b'x'); (self.work / 'a.airp').write_bytes(b'x')
        with mock.patch.object(po.shutil, 'disk_usage', return_value=shutil._ntuple_diskusage(1 << 40, 1 << 40, 300 << 20)), self.assertRaises(SystemExit) as e: self.main()
        self.assertIn('GB free', str(e.exception)); self.assertEqual(self.calls, [])
        self.assertEqual(sorted(p.name for p in self.work.iterdir()), ['bases.mtl4-json'])   # a failed run's temporaries cleared

    def failed_run(self, free_with_temporaries):   # metal-tt passes the check with plenty free, then fails having written its temporaries, which take the free space down until they are cleared; the error text and what is left in the work folder
        def fail(cmd, **kw):
            cmd = [str(c) for c in cmd]
            if '-version' in cmd or '--show-sdk-version' in cmd: return self.fake_run(cmd)
            (self.work / 'translate-0001').mkdir(); (self.work / 'translate-0001/x').write_bytes(b'x'); (self.work / 'a.airp').write_bytes(b'x')
            return subprocess.CompletedProcess(cmd, 1)
        usage = lambda _: shutil._ntuple_diskusage(1 << 40, 1 << 40, free_with_temporaries if (self.work / 'a.airp').exists() else 100 << 30)
        with mock.patch.object(po.subprocess, 'run', fail), mock.patch.object(po.shutil, 'disk_usage', usage), \
                mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as e:
            po.main(['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work)])
        return str(e.exception), {p.name for p in self.work.iterdir()}

    def test_metal_tt_failure_on_a_full_volume_says_so(self):   # the free space as it failed, not after its temporaries are cleared: clearing them brings it back above the estimate
        text, left = self.failed_run(100 << 20)
        self.assertIn('0.1 GB free', text); self.assertIn('likely out of disk space', text)
        self.assertFalse({'translate-0001', 'a.airp'} & left)

    def test_metal_tt_failure_with_room_does_not_blame_the_disk(self):
        text, left = self.failed_run(10 << 30)
        self.assertNotIn('out of disk space', text); self.assertIn('metal-tt failed', text)
        self.assertFalse({'translate-0001', 'a.airp'} & left)


class Default(unittest.TestCase):
    """Which games get an archive without DM_PSO_GPUS: build_ipa.sh's own lines, run with game.py's default form, so only a profile with pso_mtl4 on (the Metal 4 path, which alone
    reads the archive on iOS) gets one — g18p by default, the profile key pso_gpu's architectures when it names them (joined with commas) and g18p on a key the shell guard does not take —
    and an explicit DM_PSO_GPUS wins, also when empty"""

    def lines(self):
        lines = (REPO / 'dm/ios/tools/build_ipa.sh').read_text().splitlines()
        i = next(n for n, ln in enumerate(lines) if ln.startswith('PSO=; P4='))
        return 'die() { echo "FAIL: $*"; exit 1; }\nt() { echo "$*" >&2; }\n' + '\n'.join(lines[i:i + 4])   # the P4 / pso_gpu reads, the key's shell guard and the GPUS chain

    def gpus(self, profile, **env):
        with tempfile.TemporaryDirectory() as d:
            for k, v in profile.items(): Path(d, k).mkdir(); Path(d, k, 'game.tsv').write_text(v)
            e = {k: v for k, v in os.environ.items() if k != 'DM_PSO_GPUS'}
            e.update(env, DM=str(REPO / 'dm'), DM_GAMES_DIR=d, GD=d)
            return {gid: (r.stdout, r.stderr) for gid, r in
                    ((gid, subprocess.run(['bash', '-c', self.lines() + '\necho "$P4 $GPUS"'], env=dict(e, GAME=gid),
                                          capture_output=True, text=True)) for gid in profile)}

    def test_by_profile(self):
        games = {'0100000000F0F000': 'title_id\t0100000000F0F000\npso_mtl4\ton\n',
                 '0100000000F0F001': 'title_id\t0100000000F0F001\n',
                 '0100000000F0F002': 'title_id\t0100000000F0F002\npso_mtl4\toff\n',
                 '0100000000F0F003': 'title_id\t0100000000F0F003\npso_mtl4\ton\npso_gpu\tapplegpu_g19p\n',
                 '0100000000F0F004': 'title_id\t0100000000F0F004\npso_mtl4\ton\npso_gpu\tg18p   g19p\n',
                 '0100000000F0F005': 'title_id\t0100000000F0F005\npso_mtl4\ton\npso_gpu\tnone\n',                                   # none is no token of this key
                 '0100000000F0F006': 'title_id\t0100000000F0F006\npso_mtl4\ton\npso_gpu\tg15s g16s g17p g18p g19p\n',               # 5 architectures: more than the key takes
                 '0100000000F0F007': 'title_id\t0100000000F0F007\npso_mtl4\toff\npso_gpu\tg19p\n'}
        r = self.gpus(games)
        self.assertEqual({k: p[0].strip() for k, p in r.items()},
                         {'0100000000F0F000': 'on g18p', '0100000000F0F001': 'off none', '0100000000F0F002': 'off none',
                          '0100000000F0F003': 'on applegpu_g19p', '0100000000F0F004': 'on g18p,g19p', '0100000000F0F005': 'on g18p',
                          '0100000000F0F006': 'on g18p', '0100000000F0F007': 'off none'})
        self.assertEqual({k for k, p in r.items() if 'taking g18p' in p[1]}, {'0100000000F0F005', '0100000000F0F006'})   # a key the guard rejects keeps the default, and says so
        self.assertEqual({k: p[0].strip() for k, p in self.gpus(games, DM_PSO_GPUS='g17p').items()},
                         {k: v.split()[0] + ' g17p' for k, v in {k: p[0].strip() for k, p in r.items()}.items()})
        self.assertEqual(self.gpus(games, DM_PSO_GPUS='none')['0100000000F0F000'][0].strip(), 'on none')
        self.assertEqual(self.gpus(games, DM_PSO_GPUS='')['0100000000F0F000'][0].strip(), 'on')

    def test_nsp_defaults_follow_the_same_profile_gate(self):
        # Run the NSP script's actual choice block without extracting or building a game.
        script = (REPO / 'scripts/build_ipa_from_nsp.py').read_text()
        start = script.index("    gpus = (args.gpu or PSO_GPUS).strip()")
        end = script.index("    ipa_env['DM_PSO_GPUS'] = gpus", start)
        block = '\n'.join(line[4:] for line in script[start:end].splitlines())
        for p4, pg, expected in [('on', '', 'g18p'), ('on', 'g18p   applegpu_g19p', 'g18p,applegpu_g19p'),
                                 ('off', '', 'none'), ('off', 'g19p', 'none')]:
            for explicit, setting in [('', ''), ('g17p', ''), ('none', ''), ('', 'g16p')]:
                with self.subTest(pso_mtl4=p4, pso_gpu=pg, gpu=explicit, setting=setting):
                    scope = {'args': types.SimpleNamespace(gpu=explicit), 'PSO_GPUS': setting,
                             'g': types.SimpleNamespace(get={'pso_mtl4': p4, 'pso_gpu': pg}.get),
                             're': re, 'BuildError': RuntimeError}
                    exec(block, scope)
                    self.assertEqual(scope['gpus'], explicit or setting or expected)

    def test_breath_of_the_wild(self):   # the profile that turns the Metal 4 path on: its app gets the archive
        r = subprocess.run([sys.executable, str(REPO / 'dm/games/game.py'), '01007EF00011E000', 'pso_mtl4', 'off'], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), 'on')


if __name__ == '__main__':
    unittest.main()
