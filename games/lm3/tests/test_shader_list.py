"""The public LM3 list carries metadata only and preserves the v4 binding ABI."""
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('lm3_shader_list', ROOT / 'scripts/shader_list.py')
shader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shader)


class ShaderIdentifiers(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='lm3 shader identifiers ')
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.cache = self.root / 'cache'
        self.cache.mkdir()
        self.list = self.root / 'shader_list.tsv'
        self.vs = self.variant('1' * 64, 'vs', {}, 'a' * 12)
        self.fs = self.variant('2' * 64, 'ps', {'fout': {'0': 'uint'}}, 'b' * 12)
        self.cs = self.variant('3' * 64, 'cs', {'img': {'1': {'type': '2d', 'fmt': 'r32ui'}}}, 'c' * 12)

    def variant(self, key, stage, values, nonce):
        deps = {kind: {} for kind in shader.DEFAULTS}
        deps.update(values)
        name = key + '-' + nonce
        (self.cache / (key + '.variants')).write_text(json.dumps([{'variant': name, 'deps': deps}]))
        (self.cache / (name + '_b.json')).write_text(json.dumps({
            'key': name + '_b', 'route': 'B', 'chain': shader.CHAIN, 'stage': stage, 'deps': deps}))
        return name + '_b'

    def render(self, vs=None, fs=None):
        data = bytearray(shader.SKEY_BYTES)
        data[:80] = (vs or self.vs).encode().ljust(80, b'\0')
        data[80:160] = (fs or self.fs).encode().ljust(80, b'\0')
        data[160], data[161] = 0, 1
        return data.hex()

    def recipes(self):
        path = self.root / 'pso_user.tsv'
        path.write_text(f'S\t{self.vs}\t0\nS\t{self.fs}\t1\nC\t{self.cs}\nR\t{self.render()}\n')
        return path

    def merged(self):
        return shader.merge(self.list, [self.cache], [self.recipes()])

    def test_default_equivalent_alias_is_not_contributed(self):
        result = self.merged()
        self.assertEqual(len(result.variants), 2)
        self.assertNotIn('1' * 64, {key for key, _ in result.variants})
        vertex = next(row[1] for row in result.warm if row[0] == 'S' and row[2] == '0')
        self.assertEqual(vertex, '1' * 64 + '-' + shader.DEFAULT_SUFFIX + '_b')

    def test_v4_matcher_keeps_integer_outputs_and_attributes(self):
        self.assertFalse(shader.serves(shader.dependencies({'fout': {'0': 'uint'}}), shader.EMPTY))
        self.assertFalse(shader.serves(shader.dependencies({'attr': {'1': 'sint'}}), shader.EMPTY))
        self.assertTrue(shader.serves(shader.dependencies({'tex': {'0': '2d'}, 'fout': {'0': 'float'}}), shader.EMPTY))

    def test_roundtrip_idempotence_and_export_resolves_actual_cache_aliases(self):
        result = self.merged()
        text = result.serialize()
        shader.atomic_text(self.list, text)
        self.assertEqual(shader.ShaderList.load(self.list).serialize(), text)
        self.assertEqual(self.merged().serialize(), text)
        exported, skipped = shader.export(result, [self.cache])
        self.assertEqual(skipped, 0)
        self.assertIn('S\t' + self.vs + '\t0', exported)
        self.assertIn('R\t' + self.render(), exported)

    def test_no_shader_or_game_bytes_are_read_or_exported(self):
        marker = 'SHADER_CODE_MUST_STAY_LOCAL'
        for extension in ('.metal', '.metallib', '.spv', '.code', '.save'):
            (self.cache / ('unused' + extension)).write_text(marker)
        result = self.merged()
        text = result.serialize()
        self.assertNotIn(marker, text)
        self.assertNotIn(str(self.cache), text)
        self.assertEqual({line[0] for line in text.splitlines() if not line.startswith('#')}, set('VASCR'))

    def test_missing_reference_or_wrong_shader_stage_is_rejected(self):
        for text in ('S\t' + '9' * 64 + '-' + 'a' * 12 + '_b\t0\n', 'C\t' + self.fs + '\n'):
            path = self.root / 'bad.tsv';path.write_text(text)
            with self.assertRaisesRegex(ValueError, 'missing shader|wrong stage'):
                shader.merge(self.list, [self.cache], [path])

    def test_foreign_variant_index_and_translation_chain_are_rejected(self):
        path = self.cache / ('1' * 64 + '.variants')
        row = json.loads(path.read_text())
        row[0]['variant'] = '7' * 64 + '-' + 'a' * 12
        path.write_text(json.dumps(row))
        with self.assertRaisesRegex(ValueError, 'another shader'):
            self.merged()
        row[0]['variant'] = self.vs[:-2];path.write_text(json.dumps(row))
        reflection = self.cache / (self.fs + '.json')
        data = json.loads(reflection.read_text());data['chain'] = 'another-game-chain'
        reflection.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'translation-chain'):
            self.merged()

    def test_process_pointers_or_nonzero_key_padding_are_rejected(self):
        for offset in (*shader.POINTER_OFFSETS, 198):
            data = bytearray.fromhex(self.render());data[offset] = 1
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, 'pointer|padding'):
                shader.pipeline(data.hex())

    def test_blend_reserved_bits_and_undefined_enums_are_rejected(self):
        for word in (1 << 47, 1 << 37, 3 << 44):
            data = bytearray.fromhex(self.render());data[272:280] = word.to_bytes(8, 'little')
            with self.subTest(word=word), self.assertRaisesRegex(ValueError, 'blend'):
                shader.pipeline(data.hex())
        data = bytearray.fromhex(self.render())
        data[272:280] = ((22 << 37) | (1 << 43)).to_bytes(8, 'little')
        shader.pipeline(data.hex())  # Known SCREEN extension, not a promise of base-runtime support.

    def test_active_attribute_stream_must_fit_the_runtime_array(self):
        for stream in (16, 31):
            data = bytearray.fromhex(self.render());data[336:340] = (1 | stream << 27).to_bytes(4, 'little')
            with self.subTest(stream=stream), self.assertRaisesRegex(ValueError, 'stream'):
                shader.pipeline(data.hex())
        data[336:340] = (1 | 15 << 27).to_bytes(4, 'little')
        shader.pipeline(data.hex())

    def test_attachment_mapping_is_bounded_and_only_present_when_merged(self):
        data = bytearray.fromhex(self.render());data[528] = 1
        with self.assertRaisesRegex(ValueError, 'unmerged'):
            shader.pipeline(data.hex())
        data[162] = 1;data[528] = 8
        with self.assertRaisesRegex(ValueError, 'out-of-bounds'):
            shader.pipeline(data.hex())
        data[528] = 255
        shader.pipeline(data.hex())

    def test_specialization_descriptor_has_bounded_attachments_and_matching_mask(self):
        data = bytearray.fromhex(self.render())
        data[162:164] = b'\1\1';data[164:198] = bytes((1, 1, 0)) + b'\xff' * 31
        shader.pipeline(data.hex())
        for offset, value in ((166, 8), (165, 2), (164, 2), (162, 0)):
            bad = data.copy();bad[offset] = value
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, 'specialization'):
                shader.pipeline(bad.hex())
        # A merged unspecialized draw may retain the previous valid kind=1 descriptor.
        data[163] = 0
        shader.pipeline(data.hex())
        data[164:198] = bytes(34);data[165] = 1
        with self.assertRaisesRegex(ValueError, 'inactive'):
            shader.pipeline(data.hex())

    def test_active_specialization_requires_a_fragment_shader(self):
        data = bytearray.fromhex(self.render())
        data[80:160] = bytes(80);data[161] = 0
        data[162:164] = b'\1\1';data[164:198] = bytes((1, 0)) + b'\xff' * 32
        with self.assertRaisesRegex(ValueError, 'fragment shader'):
            shader.pipeline(data.hex())

    def test_unterminated_names_bad_stage_and_wrong_abi_are_rejected(self):
        bad = bytearray.fromhex(self.render());bad[:80] = b'x' * 80
        with self.assertRaisesRegex(ValueError, 'shader-name'):
            shader.pipeline(bad.hex())
        bad = bytearray.fromhex(self.render());bad[161] = 5
        with self.assertRaisesRegex(ValueError, 'stage'):
            shader.pipeline(bad.hex())
        with self.assertRaisesRegex(ValueError, '544-byte ABI'):
            shader.pipeline('00' * 543)

    def test_list_rejects_wrong_namespace_raw_shader_payload_and_unbound_recipe(self):
        text = self.merged().serialize()
        for changed in (text.replace('# LM3 shader list v1', '# TOTK shader list v1', 1),
                        text + 'metal\tfragment main(){}\n',
                        text + 'S\t' + '9' * 64 + '-' + 'f' * 12 + '_b\t0\tm\n'):
            self.list.write_text(changed)
            with self.assertRaises(ValueError):
                shader.ShaderList.load(self.list)

    def test_duplicate_json_fields_and_non_type_answers_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            shader.parse_json('{"tex":{},"tex":{}}')
        for value in ({'tex': {'0': 'fragment main(){}'}}, {'attr': {'0': 'double'}}, {'unknown': {}}):
            with self.assertRaises(ValueError):
                shader.dependencies(value)

    def test_export_skips_unavailable_cache_versions_without_fabricating_shaders(self):
        result = self.merged()
        (self.cache / ('2' * 64 + '.variants')).unlink()
        text, skipped = shader.export(result, [self.cache])
        self.assertEqual(skipped, 2)
        self.assertNotIn('R\t', text)
        self.assertNotIn(self.fs, text)

    def test_export_never_resolves_a_shader_of_the_wrong_stage(self):
        result = self.merged()
        reflection = self.cache / (self.fs + '.json')
        value = json.loads(reflection.read_text());value['stage'] = 'vs'
        reflection.write_text(json.dumps(value))
        text, skipped = shader.export(result, [self.cache])
        self.assertEqual(skipped, 2)
        self.assertNotIn('R\t', text)

    def test_flags_are_merged_without_duplicate_warm_rows(self):
        result = self.merged()
        row = next(iter(result.warm))
        result.warm[row] = 't'
        shader.atomic_text(self.list, result.serialize())
        merged = self.merged()
        self.assertEqual(merged.warm[row], 'tm')
        self.assertEqual(len(merged.warm), 4)

    def test_output_cannot_replace_metadata_inputs(self):
        with self.assertRaisesRegex(ValueError, 'read-only shader cache'):
            shader.protect_inputs(self.cache / 'export.tsv', [self.cache])
        with self.assertRaisesRegex(ValueError, 'replace an input'):
            shader.protect_inputs(self.list, [self.cache], [self.list])

    def test_translation_chain_matches_current_lm3_runtime(self):
        text = (ROOT / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text()
        actual = re.search(r'#define DMSH_CHAIN "([^"]+)"', text)[1]
        self.assertEqual(actual, shader.CHAIN, 'Review matching semantics and pipeline ABI for the new chain')

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'macOS C ABI check')
    def test_actual_runtime_pipeline_abi_matches_serializer(self):
        source = self.root / 'abi.c';binary = self.root / 'abi'
        source.write_text('''#include <stddef.h>
#include <stdio.h>
typedef struct { char key[80]; int stage; } NvShader;
#include "nvn_pso_skey.h"
int main(void) {
 printf("%zu %zu %zu %zu %zu %zu %zu\\n",sizeof(pso_skey),offsetof(pso_skey,k)+offsetof(pso_key,vs),
   offsetof(pso_skey,k)+offsetof(pso_key,fs),offsetof(pso_skey,k)+offsetof(pso_key,fsfn),
   offsetof(pso_skey,k)+offsetof(pso_key,blend),offsetof(pso_skey,k)+offsetof(pso_key,attr),
   offsetof(pso_skey,k)+offsetof(pso_key,lmap));
 return 0;
}
''')
        folder = ROOT / 'third_party/d2r_gfx/gfx/nvn'
        subprocess.run(['xcrun', 'clang', '-std=c11', '-I', str(folder), '-I', str(folder / 'include'),
                        str(source), '-o', str(binary)], check=True, capture_output=True)
        actual = tuple(map(int, subprocess.check_output([str(binary)], text=True).split()))
        self.assertEqual(actual, (shader.SKEY_BYTES, *shader.POINTER_OFFSETS,
                                  shader.BLEND_OFFSETS.start, shader.ATTR_OFFSETS.start, 528))


if __name__ == '__main__':
    unittest.main()
