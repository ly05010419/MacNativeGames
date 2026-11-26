"""Portable seed integrity and publication, using only synthetic shader data."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('lm3_prepare_shader_seed', ROOT / 'scripts/prepare_shader_seed.py')
seed = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(seed)


def snapshots(folder):
    return {str(path.relative_to(folder)): path.read_bytes() for path in folder.rglob('*') if path.is_file()}


class ShaderSeed(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lm3-shader-seed-')
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.cache = self.work / 'cache'
        self.cache.mkdir()
        self.output = self.work / 'bundle' / 'shader-seed'

    def variant(self, cache=None, *, shader='a' * 64, suffix='1' * 12, stage='vs', deps=None):
        cache = cache or self.cache
        cache.mkdir(exist_ok=True)
        variant = shader + '-' + suffix
        deps = deps if deps is not None else {'tex': {}, 'img': {}, 'attr': {}, 'fout': {}}
        index = cache / (shader + '.variants')
        rows = json.loads(index.read_text()) if index.exists() else []
        rows.append({'variant': variant, 'deps': deps})
        index.write_text(json.dumps(rows))
        reflection = {'key': variant + '_b', 'route': 'B', 'chain': seed.current_chain(), 'stage': stage,
                      'entry': 'main0', 'deps': deps, 'bindings': [], 'vertex_inputs': [], 'c1': ''}
        (cache / (variant + '_b.json')).write_text(json.dumps(reflection))
        kind = {'vs': 'vertex', 'ps': 'fragment', 'cs': 'kernel'}[stage]
        (cache / (variant + '_b.metal')).write_text(f'{kind} void main0() {{}}\n')
        return variant

    def prepare(self, *caches, lists=()):
        return seed.prepare_seed(list(caches) or [self.cache], list(lists), self.output)

    def render(self, vertex, fragment=''):
        data = bytearray(seed.PSO_SKEY_BYTES)
        data[:len(vertex) + 2] = (vertex + '_b').encode()
        if fragment:
            data[80:80 + len(fragment) + 2] = (fragment + '_b').encode()
            data[161] = 1
        return data

    def test_valid_seed_has_only_verified_portable_files_and_no_personal_data(self):
        variant = self.variant()
        for name in ('private.save', 'prod.keys', variant + '.code', variant + '.spv', variant + '_b.metallib'):
            (self.cache / name).write_bytes(b'excluded data')
        before = snapshots(self.cache)
        manifest = self.prepare()
        self.assertEqual(manifest['schema'], 1)
        self.assertEqual(manifest['chain'], seed.current_chain())
        self.assertEqual(manifest['shaders'], 1)
        self.assertEqual(set(manifest['files']), {
            'a' * 64 + '.variants', variant + '_b.metal', variant + '_b.json', 'pso_manifest.tsv'})
        for name, digest in manifest['files'].items():
            self.assertEqual(hashlib.sha256((self.output / name).read_bytes()).hexdigest(), digest)
        self.assertEqual(set(snapshots(self.output)), set(manifest['files']) | {'seed-manifest.json'})
        self.assertEqual(snapshots(self.cache), before)

    def test_merge_deduplicates_variants_and_is_order_independent(self):
        first = self.variant(deps={'tex': {'0': 'cube'}, 'img': {}, 'attr': {}, 'fout': {}})
        other = self.work / 'second-cache'
        shutil.copytree(self.cache, other)
        second = self.variant(other, suffix='2' * 12)
        self.prepare(self.cache, other)
        before = snapshots(self.output)
        self.prepare(other, self.cache, self.cache)
        self.assertEqual(snapshots(self.output), before)
        rows = json.loads((self.output / ('a' * 64 + '.variants')).read_text())
        self.assertEqual([row['variant'] for row in rows], [first, second])

    def test_conflicting_source_or_metadata_rejected_without_destroying_old_seed(self):
        variant = self.variant()
        self.prepare()
        old = snapshots(self.output)
        other = self.work / 'conflict'
        shutil.copytree(self.cache, other)
        (other / (variant + '_b.metal')).write_text('vertex void main0() { /* different */ }')
        with self.assertRaisesRegex(ValueError, 'conflicting shader contents'):
            self.prepare(self.cache, other)
        self.assertEqual(snapshots(self.output), old)
        shutil.rmtree(other)
        shutil.copytree(self.cache, other)
        reflection_file = other / (variant + '_b.json')
        reflection = json.loads(reflection_file.read_text())
        reflection['deps']['tex']['0'] = 'cube'
        reflection_file.write_text(json.dumps(reflection))
        (other / ('a' * 64 + '.variants')).write_text(json.dumps([
            {'variant': variant, 'deps': reflection['deps']}]))
        with self.assertRaisesRegex(ValueError, 'conflicting variant index metadata'):
            self.prepare(self.cache, other)
        self.assertEqual(snapshots(self.output), old)

    def test_bad_indexes_chain_dependencies_and_missing_peer_are_rejected(self):
        cases = ('non-array', 'invalid-json', 'duplicate-json-key', 'cross-shader', 'unsafe-name',
                 'wrong-chain', 'wrong-reflection-key', 'wrong-deps', 'bad-deps-type', 'missing-metal', 'symlink')
        for case in cases:
            with self.subTest(case=case):
                cache = self.work / case
                variant = self.variant(cache)
                index = cache / ('a' * 64 + '.variants')
                reflection_file = cache / (variant + '_b.json')
                reflection = json.loads(reflection_file.read_text())
                rows = json.loads(index.read_text())
                if case == 'non-array':
                    index.write_text('{}')
                elif case == 'invalid-json':
                    index.write_text('[{')
                elif case == 'duplicate-json-key':
                    index.write_text('[{"variant":"x","variant":"y","deps":{}}]')
                elif case == 'cross-shader':
                    rows[0]['variant'] = 'b' * 64 + '-'+ '1' * 12
                    index.write_text(json.dumps(rows))
                elif case == 'unsafe-name':
                    index.rename(cache / '../escape.variants')
                    (cache / 'unsafe.variants').write_text('[]')
                elif case == 'wrong-chain':
                    reflection['chain'] = 'another-chain'
                    reflection_file.write_text(json.dumps(reflection))
                elif case == 'wrong-reflection-key':
                    reflection['key'] = 'b' * 64 + '-' + '1' * 12 + '_b'
                    reflection_file.write_text(json.dumps(reflection))
                elif case == 'wrong-deps':
                    reflection['deps']['tex']['0'] = '3d'
                    reflection_file.write_text(json.dumps(reflection))
                elif case == 'bad-deps-type':
                    rows[0]['deps']['attr']['0'] = {}
                    index.write_text(json.dumps(rows))
                elif case == 'missing-metal':
                    (cache / (variant + '_b.metal')).unlink()
                elif case == 'symlink':
                    metal = cache / (variant + '_b.metal')
                    metal.unlink()
                    metal.symlink_to(reflection_file)
                with self.assertRaises(ValueError):
                    self.prepare(cache)
                self.assertFalse(self.output.exists())

    def test_pso_merges_valid_shader_render_compute_rows_and_cache_discovery(self):
        vertex = self.variant()
        fragment = self.variant(shader='b' * 64, stage='ps')
        compute = self.variant(shader='c' * 64, stage='cs')
        render = 'R\t' + self.render(vertex, fragment).hex()
        rows = [f'S\t{vertex}_b\t0', f'C\t{compute}_b', render]
        (self.cache / 'pso_collected.tsv').write_text('\n'.join(rows) + '\n')
        extra = self.work / 'profile-pso.tsv'
        extra.write_text('# ignored comment\n' + render + '\n')
        self.prepare(lists=[extra])
        out = (self.output / 'pso_manifest.tsv').read_text().splitlines()
        self.assertEqual(out.count(render), 1)
        self.assertIn(f'C\t{compute}_b', out)
        self.assertIn(f'S\t{fragment}_b\t1', out)

    def test_invalid_pso_abi_stages_keys_flags_and_pointers_are_rejected(self):
        vertex = self.variant()
        data = self.render(vertex)
        cases = [f'S\t{vertex}_b\t1', f'C\t{vertex}_b', 'X\tunexpected', 'R\t00',
                 'S\t' + 'f' * 64 + '-' + '1' * 12 + '_b\t0']
        for offset in (*seed.PSO_POINTER_OFFSETS, 162, 198):
            changed = bytearray(data)
            changed[offset] = 2
            cases.append('R\t' + changed.hex())
        for row in cases:
            with self.subTest(row=row[:80]):
                extra = self.work / 'bad-pso.tsv'
                extra.write_text(row + '\n')
                with self.assertRaises(ValueError):
                    self.prepare(lists=[extra])

    def test_publication_failure_keeps_previous_seed_and_cleans_staging(self):
        self.variant()
        self.prepare()
        old = snapshots(self.output)
        self.variant(suffix='2' * 12)
        with patch.object(seed, 'exchange_directories', side_effect=OSError('simulated exchange failure')):
            with self.assertRaisesRegex(OSError, 'simulated exchange failure'):
                self.prepare()
        self.assertEqual(snapshots(self.output), old)
        self.assertEqual(list(self.output.parent.glob('.shader-seed.stage-*')), [])
        self.prepare()
        self.assertEqual(json.loads((self.output / 'seed-manifest.json').read_text())['shaders'], 2)

    def test_input_overlap_is_rejected_without_touching_inputs(self):
        self.variant()
        before = snapshots(self.cache)
        for output in (self.cache, self.cache / 'nested', self.work):
            with self.subTest(output=output), self.assertRaisesRegex(ValueError, 'overlap'):
                seed.prepare_seed([self.cache], [], output)
        self.assertEqual(snapshots(self.cache), before)

    @unittest.skipUnless(sys.platform == 'darwin', 'actual macOS arm64 renderer ABI')
    def test_render_pso_layout_matches_current_runtime_header(self):
        source = self.work / 'abi.c'
        binary = self.work / 'abi'
        source.write_text('''#include <stddef.h>
#include <stdio.h>
typedef struct { char key[80]; int stage; } NvShader;
#include "nvn_pso_skey.h"
int main(void) {
 printf("%zu %zu %zu %zu %zu\\n", sizeof(pso_skey), offsetof(pso_skey,k),
   offsetof(pso_skey,k)+offsetof(pso_key,vs), offsetof(pso_skey,k)+offsetof(pso_key,fs),
   offsetof(pso_skey,k)+offsetof(pso_key,fsfn));
 return 0;
}
''')
        folder = ROOT / 'third_party/d2r_gfx/gfx/nvn'
        subprocess.run(['xcrun', 'clang', '-std=c11', '-I', str(folder), '-I', str(folder / 'include'),
                        str(source), '-o', str(binary)], check=True, capture_output=True)
        actual = tuple(map(int, subprocess.check_output([str(binary)], text=True).split()))
        self.assertEqual(actual, (seed.PSO_SKEY_BYTES, seed.PSO_KEY_OFFSET, *seed.PSO_POINTER_OFFSETS))


if __name__ == '__main__':
    unittest.main()
