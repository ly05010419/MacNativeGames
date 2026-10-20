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
VARIANT = KEY+'-'+'b'*12+'_b'
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


if __name__ == '__main__':
    unittest.main()
