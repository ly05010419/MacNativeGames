"""The iPhone build of Ocarina of Time can carry only the shaders its pipeline lists use (user 2026-11-09)."""
import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'games/oot/ios/tools'
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location('oot_build_ipa', TOOLS / 'build_ipa.py')
build_ipa = importlib.util.module_from_spec(spec)
# build_ipa.py imports compile_seed by name: load this game's copy, not one another game's tests imported first,
# and leave sys.modules as it was for them.
_shared = sys.modules.pop('compile_seed', None)
try:
    spec.loader.exec_module(build_ipa)
finally:
    sys.modules.pop('compile_seed', None)
    if _shared is not None:
        sys.modules['compile_seed'] = _shared

A, B = 'a' * 64, 'b' * 64
KEPT, OTHER, DROPPED = f'{A}-{"1" * 12}_b', f'{A}-{"2" * 12}_b', f'{B}-{"1" * 12}_b'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ListedNames(unittest.TestCase):
    def test_reads_the_stage_names_inside_pipeline_keys(self):
        """An R row names its two shaders only inside the hex pipeline key (shader_list.py row_variants)."""
        key = KEPT.encode().ljust(80, b'\0') + OTHER.encode().ljust(80, b'\0') + b'\x01\x02'
        with tempfile.TemporaryDirectory() as tmp:
            lists, seed, shared = Path(tmp) / 'lists', Path(tmp) / 'seed', Path(tmp) / 'shader_list.tsv'
            lists.mkdir(); seed.mkdir(); shared.write_text('')
            (lists / 'pso_list.tsv').write_text('# generated\nR\t' + key.hex() + '\n')
            (lists / 'pso_title.tsv').write_text('# generated\n')
            self.assertEqual(build_ipa.listed_names(lists, shared, seed), {KEPT, OTHER})


class SeedPrune(unittest.TestCase):
    def make_seed(self, root):
        seed = Path(root)
        for shader in (KEPT, OTHER, DROPPED):
            (seed / f'{shader}.metal').write_text('msl')
            (seed / f'{shader}.json').write_text('{}')
        (seed / f'{A}.variants').write_text(json.dumps([{'variant': KEPT[:-2]}, {'variant': OTHER[:-2]}]))
        (seed / f'{B}.variants').write_text(json.dumps([{'variant': DROPPED[:-2]}]))
        (seed / 'pso_manifest.tsv').write_text(f'R\t{KEPT}:{KEPT}\tm\nR\t{KEPT}:{DROPPED}\tm\n')
        (seed / 'shader-keys.json').write_text('{"keys": []}')
        files = {p.name: sha(p) for p in seed.iterdir()}
        (seed / 'seed-manifest.json').write_text(json.dumps({'chain': 'x', 'files': files}))
        return seed

    def test_files_of_a_kept_shader_stay_and_others_go(self):
        files = {f'{KEPT}.metal': '1', f'{OTHER}.json': '2', f'{A}.variants': '3', f'{B}.variants': '4', 'pso_title.tsv': '5'}
        self.assertEqual(sorted(build_ipa.listed_files(files, {KEPT})), sorted([f'{KEPT}.metal', f'{A}.variants', 'pso_title.tsv']))

    def test_prune_keeps_the_seed_consistent(self):
        with tempfile.TemporaryDirectory() as root:
            seed = self.make_seed(root)
            build_ipa.prune_seed(seed, {KEPT})
            manifest = json.loads((seed / 'seed-manifest.json').read_text())
            self.assertFalse((seed / f'{OTHER}.metal').exists())
            self.assertFalse((seed / f'{DROPPED}.json').exists())
            self.assertFalse((seed / f'{B}.variants').exists())
            self.assertEqual([r['variant'] for r in json.loads((seed / f'{A}.variants').read_text())], [KEPT[:-2]])
            self.assertEqual((seed / 'pso_manifest.tsv').read_text(), f'R\t{KEPT}:{KEPT}\tm\n')
            self.assertEqual(manifest['listed_shaders_only'], 1)
            for name, digest in manifest['files'].items():
                self.assertEqual(digest, sha(seed / name), name)
            self.assertEqual(sorted(p.name for p in seed.iterdir() if p.name != 'seed-manifest.json'), sorted(manifest['files']))


if __name__ == '__main__':
    unittest.main()
