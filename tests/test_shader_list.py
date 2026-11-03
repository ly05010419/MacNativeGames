"""The shared shader list (games/totk/assets/shader_list.tsv): one list for every platform and every player."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT/'games/totk/scripts/shader_list.py'
KEY = 'a'*64
VARIANT = KEY+'-b7530f698e76_b'   # the default bind state's name (prepare_shader_seed.py SUFFIX)
PIPELINE = '3'*80


def cache(folder, rows_by_key):
    folder.mkdir(parents=True, exist_ok=True)
    for key, rows in rows_by_key.items():
        (folder/f'{key}.variants').write_text(json.dumps(rows))
    return folder


def rows(path, kind=None):
    return [l for l in Path(path).read_text().splitlines() if l and not l.startswith('#') and (kind is None or l[0] == kind)]


class ShaderList(unittest.TestCase):
    def run_tool(self, *args, good=True):
        r = subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0 if good else 1, r.stderr)
        return r.stdout

    def test_resolve_names_each_variant_as_the_versions_seed_serves_its_bind_state(self):
        sys.path.insert(0, str(SCRIPT.parent))
        from prepare_shader_seed import SUFFIX, suffix, answers_for
        mine, two, gone = 'a'*64, 'c'*64, 'd'*64
        attr = answers_for({'attr': {'0': 'float'}})   # a bind state that made a variant of its own before 2026-10-26
        cube = answers_for({'tex': {'0': 'cube'}})
        old, default = f'{mine}-{suffix(attr)}', f'{mine}-{SUFFIX}'
        def key(vs, fs):
            return (vs.encode().ljust(80, b'\0')+fs.encode().ljust(80, b'\0')+b'\x07'*40).hex()
        def a_row(answers):
            text = json.dumps(answers, sort_keys=True, separators=(',', ':'))
            return f'A\t{suffix(answers)}\t{text}\n'
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            seed = cache(root/'seed', {mine: [{'variant': default, 'deps': {}}],
                                       two: [{'variant': f'{two}-{SUFFIX}', 'deps': {'tex': {'0': '2d'}}},
                                             {'variant': f'{two}-{suffix(cube)}', 'deps': {'tex': {'0': 'cube'}}}]})
            (seed/'shader-keys.json').write_text(json.dumps({'keys': [mine, two]}))
            shared = root/'shader_list.tsv'
            plain = ('R', key(default+'_b', f'{two}-{SUFFIX}_b'))
            shared.write_text(a_row(attr) + a_row(cube)
                              + f'R\t{plain[1]}\tt\n'
                              + f'S\t{gone}-{SUFFIX}_b\t1\tm\n'                                   # another version's shader
                              + f'R\t{key(old+"_b", f"{two}-{SUFFIX}_b")}\tm\n'                     # the title's row under its old name
                              + f'R\t{key(old+"_b", f"{two}-{suffix(cube)}_b")}\tm\n'               # named as the seed serves it
                              + f'R\t{key(f"{gone}-{SUFFIX}_b", f"{two}-{SUFFIX}_b")}\tm\n'         # another version's vertex shader
                              + f'C\t{two}-{SUFFIX}_b\tm\n')
            out = self.run_tool('resolve', seed, shared, root/'pso_manifest.tsv', root/'pso_title.tsv')
            self.assertIn('2 of other game versions', out)
            self.assertEqual(rows(root/'pso_title.tsv'), ['\t'.join(plain)])
            self.assertEqual(rows(root/'pso_manifest.tsv'), [f'R\t{key(default+"_b", f"{two}-{suffix(cube)}_b")}',
                                                            f'C\t{two}-{SUFFIX}_b'])

    def test_resolve_names_a_shader_served_by_another_through_its_index(self):
        sys.path.insert(0, str(SCRIPT.parent))
        from prepare_shader_seed import SUFFIX
        first, twin = 'a'*64, 'c'*64   # a translation cannot tell them apart (prepare_shader_seed.py translation_input)
        def key(vs, fs):
            return (vs.encode().ljust(80, b'\0')+fs.encode().ljust(80, b'\0')+b'\x07'*40).hex()
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            index = [{'variant': f'{first}-{SUFFIX}', 'deps': {}}]
            seed = cache(root/'seed', {first: index, twin: index})
            (seed/'shader-keys.json').write_text(json.dumps({'keys': [first, twin]}))
            shared = root/'shader_list.tsv'
            shared.write_text(f'R\t{key(twin+"-"+SUFFIX+"_b", twin+"-"+SUFFIX+"_b")}\tm\n'
                              f'R\t{key(first+"-"+SUFFIX+"_b", first+"-"+SUFFIX+"_b")}\tm\n')
            self.run_tool('resolve', seed, shared, root/'pso_manifest.tsv', root/'pso_title.tsv')
            self.assertEqual(rows(root/'pso_manifest.tsv'), [f'R\t{key(first+"-"+SUFFIX+"_b", first+"-"+SUFFIX+"_b")}'])

    def test_the_list_keeps_the_bind_state_behind_every_name_and_refuses_rows_without_one(self):
        sys.path.insert(0, str(SCRIPT.parent))
        from prepare_shader_seed import SUFFIX, suffix, answers_for
        def key(vs, fs):
            return (vs.encode().ljust(80, b'\0')+fs.encode().ljust(80, b'\0')+b'\x07'*40).hex()
        deps = '{"tex":{"0":"cube"}}'
        named = f'{KEY}-{suffix(answers_for(json.loads(deps)))}_b'
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            shared = root/'shader_list.tsv'
            (root/'contribution.tsv').write_text(f'V\t{KEY}\t{deps}\n'                       # what a game writes with the row
                                                 f'R\t{key(KEY+"-"+SUFFIX+"_b", named)}\n'
                                                 f'R\t{key(KEY+"-"+"9"*12+"_b", named)}\n')   # a bind state it does not give
            out = self.run_tool('merge', shared, root/'contribution.tsv')
            self.assertIn('1 warm rows refused', out)
            self.assertEqual(len(rows(shared, 'R')), 1)
            self.assertEqual(rows(shared, 'A'), [f'A\t{named[65:77]}\t' + json.dumps(answers_for(json.loads(deps)), sort_keys=True, separators=(',', ':'))])
            self.run_tool('stats', shared)
            text = shared.read_text()   # prune drops the V row a seed's built-in variant serves; the bind state stays
            shared.write_text(text.replace(f'V\t{KEY}\t{deps}\n', ''))
            self.run_tool('stats', shared)
            shared.write_text(text.replace(rows(shared, 'A')[0] + '\n', '').replace(f'V\t{KEY}\t{deps}\n', ''))
            self.run_tool('stats', shared, good=False)

    def test_variants_are_the_shader_and_its_bind_state_not_a_variant_name(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root/'list.tsv'
            deps = {'tex': {'0': 'cube'}, 'attr': {}, 'img': {}, 'fout': {}}
            mac = cache(root/'mac', {KEY: [dict(variant=KEY+'-'+'1'*12, deps=deps)]})
            # the same shader and deps under another name (answers outside deps differ) is the same entry
            ipad = cache(root/'ipad', {KEY: [dict(variant=KEY+'-'+'2'*12, deps=dict(reversed(list(deps.items())))),
                                             dict(variant=KEY+'-'+'3'*12, deps={'tex': {'0': '2d'}})]})
            self.run_tool('merge', out, mac)
            self.assertIn('1 new', self.run_tool('merge', out, ipad))
            variants = rows(out, 'V')
            self.assertEqual(len(variants), 2)
            self.assertEqual(variants, sorted(variants))
            self.assertIn('0 new', self.run_tool('merge', out, mac))

    def test_warm_rows_keep_their_order_and_both_lists_come_back_out(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root/'list.tsv'
            title = root/'title.tsv'; title.write_text(f'# title\nS\t{VARIANT}\t0\nR\t{PIPELINE}\n')
            later = root/'pso_user.tsv'; later.write_text(f'R\t{"4"*80}\nR\t{PIPELINE}\nC\t{VARIANT}\n')
            self.run_tool('merge', '--title', out, title)
            self.run_tool('merge', out, later)
            self.assertEqual(rows(out, 'R'), [f'R\t{PIPELINE}\ttm', f'R\t{"4"*80}\tm'])
            manifest, first = root/'pso_manifest.tsv', root/'pso_title.tsv'
            self.run_tool('export', out, manifest, first)
            self.assertEqual(rows(first), [f'S\t{VARIANT}\t0', f'R\t{PIPELINE}'])
            self.assertEqual(rows(manifest), [f'R\t{PIPELINE}', f'R\t{"4"*80}', f'C\t{VARIANT}'])

    def test_border_recipes_travel_with_their_pipeline(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root/'list.tsv'
            fs = '3:7:0x1p-1,0x1p-1,0x1p-1,0x1p-1;'
            vs = '8:3:0x1p-2,0x1.99999ap-2,0x1.8p-1,0x1.ccccccp-1;9:7:0x0p+0,-0x1p+0,0x1p+0,0x1p+0:f5,5,5,1,3,0x1p+0,0x0p+0,0x1.ep+3;'   # a filtering sampler
            later = root/'pso_user.tsv'
            later.write_text(f'R\t{PIPELINE}\nB\t{PIPELINE}\t-\t{fs}\nB\t{PIPELINE}\t{vs}\t-\n'
                             f'B\t{PIPELINE}\t-\t-\nB\t{PIPELINE}\t-\t3:9:0x1p-1,0x1p-1,0x1p-1,0x1p-1;\nB\t{PIPELINE}\t-\t3:7:0.5,0.5,0.5,0.5;\n')
            self.assertIn('2 B', self.run_tool('merge', out, later))
            self.assertEqual(rows(out, 'B'), [f'B\t{PIPELINE}\t-\t{fs}\tm', f'B\t{PIPELINE}\t{vs}\t-\tm'])
            self.run_tool('export', out, root/'one.tsv')
            self.assertEqual(rows(root/'one.tsv', 'B'), [f'B\t{PIPELINE}\t-\t{fs}', f'B\t{PIPELINE}\t{vs}\t-'])
            self.assertIn('2 B', self.run_tool('stats', out))

    def test_tile_reads_travel_with_their_pipeline(self):
        # the variant reading the draw's attachments from the tile (nvn_mtl.m warm_tileread); only pso_user.tsv had them
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root/'list.tsv'
            later = root/'pso_user.tsv'
            later.write_text(f'R\t{PIPELINE}\nT\t{PIPELINE}\t5:3;7:1;\nT\t{PIPELINE}\t15:0;\nT\t{PIPELINE}\t5:3;7:1;\n'
                             f'T\t{PIPELINE}\t-\nT\t{PIPELINE}\t5:9;\nT\t{PIPELINE}\t5:3\nT\tnot hex\t5:3;\n')
            self.assertIn('2 T', self.run_tool('merge', out, later))
            self.assertEqual(rows(out, 'T'), [f'T\t{PIPELINE}\t5:3;7:1;\tm', f'T\t{PIPELINE}\t15:0;\tm'])
            self.run_tool('export', out, root/'one.tsv')
            self.assertEqual(rows(root/'one.tsv', 'T'), [f'T\t{PIPELINE}\t5:3;7:1;', f'T\t{PIPELINE}\t15:0;'])

    def test_a_runtime_without_a_title_list_gets_every_warm_row_in_one_file(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root/'list.tsv'
            title = root/'title.tsv'; title.write_text(f'S\t{VARIANT}\t0\n')
            later = root/'later.tsv'; later.write_text(f'R\t{PIPELINE}\n')
            self.run_tool('merge', '--title', out, title); self.run_tool('merge', out, later)
            self.run_tool('export', out, root/'one.tsv')
            self.assertEqual(rows(root/'one.tsv'), [f'S\t{VARIANT}\t0', f'R\t{PIPELINE}'])

    def test_a_translation_cache_brings_its_shaders_but_not_every_pipeline_it_built(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root/'list.tsv'
            folder = cache(root/'cache', {KEY: [dict(variant=KEY+'-'+'1'*12, deps={})]})
            (folder/'pso_collected.tsv').write_text(f'R\t{PIPELINE}\n')
            self.run_tool('merge', out, folder)
            self.assertEqual((len(rows(out, 'V')), rows(out, 'R')), (1, []))

    def test_image_slots_keep_their_type_and_format(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root/'list.tsv'
            deps = {'img': {'2': {'type': '2d', 'fmt': 'rgba8'}}, 'tex': {}, 'attr': {}, 'fout': {}}
            self.run_tool('merge', out, cache(root/'c', {KEY: [dict(variant=KEY+'-'+'5'*12, deps=deps)]}))
            self.assertEqual(len(rows(out, 'V')), 1)
            self.run_tool('stats', out)

    def test_rows_that_are_not_shaders_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root/'list.tsv'
            bad = cache(root/'bad', {'not-a-key': [dict(variant='x', deps={})],
                                     KEY: [dict(variant=KEY+'-'+'4'*12, deps=['not', 'a', 'dict'])]})
            junk = root/'junk.tsv'; junk.write_text('R\tnot hex\nS\tx_b\t9\nhello\n')
            self.assertIn('0 new', self.run_tool('merge', out, bad, junk))
            out.write_text(out.read_text() + 'garbage line\n')
            self.run_tool('stats', out, good=False)

    def test_locator_rows_say_where_a_shader_sits_in_the_players_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root/'list.tsv'
            src = root/'other.tsv'
            src.write_text(f'L\t{KEY}\tLib/agl/agl_resource.sarc.zs/agl_common.sharcb\t1a00\t2b00\t1d0\n'
                           f'L\t{KEY}\t../escape\t0\t0\t10\n'      # never a path outside the game files
                           f'L\tnot-a-key\tLib/x\t0\t0\t10\n')
            self.run_tool('merge', out, src)
            self.assertEqual(rows(out, 'L'), [f'L\t{KEY}\tLib/agl/agl_resource.sarc.zs/agl_common.sharcb\t1a00\t2b00\t1d0'])
            self.assertIn('1 located', self.run_tool('stats', out))

    def test_each_games_list_gets_its_own_header(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('shader_list', SCRIPT)
        shader_list = importlib.util.module_from_spec(spec); spec.loader.exec_module(shader_list)
        self.assertIn('TOTK shader list', shader_list.header_for(ROOT/'games/totk/assets/shader_list.tsv'))
        self.assertIn('Breath of the Wild shader list', shader_list.header_for(ROOT/'games/botw/assets/shader_list.tsv'))
        self.assertIn('TOTK shader list', shader_list.header_for('/tmp/anywhere/list.tsv'))

    def test_the_repository_list_is_valid(self):
        self.assertIn('variants', self.run_tool('stats', ROOT/'games/totk/assets/shader_list.tsv'))

    def load_module(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('shader_list', SCRIPT)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        return module

    def test_pipelines_that_differ_only_in_state_they_do_not_use_are_one_row(self):
        import struct
        def key(blend_en, chmask, blend0, blend1):   # nvn_pso_skey.h pso_skey: target 0 is RGBA8 (70), target 1 missing
            b = bytearray(544); b[0:len(VARIANT)] = VARIANT.encode(); b[80:80+len(VARIANT)] = VARIANT.encode()
            struct.pack_into('<8I', b, 216, 70, 0, 0, 0, 0, 0, 0, 0)
            struct.pack_into('<II', b, 260, blend_en, chmask); struct.pack_into('<QQ', b, 272, blend0, blend1)
            return b.hex()
        stale, other, clean = key(0b10, 0xff, 0x1234, 0x5678), key(0, 0x0f, 0x9abc, 0), key(0, 0x0f, 0, 0)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root/'list.tsv'
            (root/'pso_user.tsv').write_text(f'R\t{stale}\nR\t{other}\nR\t{clean}\nB\t{stale}\t-\t3:7:0x1p-1,0x1p-1,0x1p-1,0x1p-1;\n')
            self.run_tool('merge', out, root/'pso_user.tsv')
            unpack = self.load_module().unpack_key
            self.assertEqual([unpack(r.split('\t')[1]) for r in rows(out, 'R')], [clean])
            self.assertEqual([unpack(r.split('\t')[1]) for r in rows(out, 'B')], [clean])
        self.assertEqual(self.load_module().pipeline_key(PIPELINE), PIPELINE)   # a key of another size stays

    def test_the_repository_lists_hold_normalised_pipeline_keys(self):
        shader_list = self.load_module()
        for game in ('totk', 'botw'):
            for line in rows(ROOT/f'games/{game}/assets/shader_list.tsv'):
                f = line.split('\t')
                if f[0] in ('R', 'B', 'T'):
                    key = shader_list.unpack_key(f[1])
                    self.assertEqual(shader_list.pipeline_key(key), key, game)

    def test_full_pipeline_keys_are_kept_packed_and_exported_as_hex(self):
        import struct
        shader_list = self.load_module()
        b = bytearray(544); b[0:len(VARIANT)] = VARIANT.encode(); b[80:80+len(VARIANT)] = VARIANT.encode()
        struct.pack_into('<8I', b, 216, 70, 0x10, 0, 0, 0, 0, 0, 0); b[400:404] = bytes([1, 0, 0, 0x10]); b[543] = 9
        key = b.hex()
        packed = shader_list.pack_key(key)
        self.assertLess(len(packed), len(key) // 3)
        self.assertEqual(shader_list.unpack_key(packed), key)
        self.assertEqual(shader_list.pack_key(PIPELINE), PIPELINE)   # a key of another size stays hex
        self.assertEqual(shader_list.unpack_key(packed[:-3]), packed[:-3])   # a damaged one does not unpack to a short key
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); out = root/'list.tsv'
            (root/'pso_user.tsv').write_text(f'R\t{key}\nT\t{key}\t5:3;\n')
            self.run_tool('merge', out, root/'pso_user.tsv')
            self.assertEqual(rows(out, 'R'), [f'R\t{packed}\tm'])
            self.assertEqual(rows(out, 'T'), [f'T\t{packed}\t5:3;\tm'])
            self.run_tool('merge', root/'again.tsv', out)   # a packed list reads back as the same rows
            self.assertEqual(rows(root/'again.tsv'), rows(out))
            self.run_tool('export', out, root/'one.tsv')
            self.assertEqual(rows(root/'one.tsv'), [f'R\t{key}', f'T\t{key}\t5:3;'])
            other = root/'games/odyssey/assets/shader_list.tsv'; other.parent.mkdir(parents=True)
            self.run_tool('merge', other, root/'pso_user.tsv')   # other games' own tools read hex
            self.assertEqual(rows(other, 'R'), [f'R\t{key}\tm'])


if __name__ == '__main__':
    unittest.main()
