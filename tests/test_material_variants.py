"""Research scanner coverage must match the runtime, not just exact JSON rows."""
import hashlib
import json
from pathlib import Path
import random
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'games/totk/scripts'))
from prepare_shader_seed import CHAIN
from read_material_metadata import select_programs
from find_material_variants import Seed, deps_match


class OfflineMaterialScanTests(unittest.TestCase):
    def test_runtime_defaults(self):
        self.assertTrue(deps_match(dict(attr={'0': 'float'}, tex={'3': '2d'}, img={'0': {}}), {}))
        self.assertFalse(deps_match(dict(attr={'0': 'uint'}), {}))

    def test_asymmetric_dependencies(self):
        self.assertTrue(deps_match(dict(attr={'0': 'float'}), dict(attr={'0': 'float', '6': 'uint'})))
        self.assertFalse(deps_match(dict(attr={'6': 'float'}), dict(attr={'0': 'float', '6': 'uint'})))

    def test_image_formats_are_not_ignored(self):
        self.assertFalse(deps_match(dict(img={'1': dict(type='2d', fmt='r32ui')}),
                                    dict(img={'1': dict(type='2d', fmt='r32f')})))

    def test_bitset_selection_equals_brute_force(self):
        rng = random.Random(42)
        keys = [tuple(rng.randrange(256) for _ in range(3)) for _ in range(301)]
        buckets = {}
        for _ in range(30):
            constraints = [(rng.randrange(3), 15 << shift, shift, rng.randrange(16))
                           for shift in (0, 4)]
            expected = [i for i, key in enumerate(keys)
                        if all((key[w] & mask) >> shift == choice for w, mask, shift, choice in constraints)]
            self.assertEqual(select_programs(keys, constraints, buckets), expected)

    def test_empty_constraints_select_all(self):
        self.assertEqual(select_programs([(0,), (1,)], [], {}), [0, 1])
        self.assertEqual(select_programs([], [], {}), [])

    def test_seed_rejects_other_chain(self):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            (p / 'seed-manifest.json').write_text(json.dumps(dict(chain='old', source_romfs_sha256='a')))
            with self.assertRaisesRegex(ValueError, 'chain'):
                Seed(p, 'a')

    def test_seed_rejects_other_game_data(self):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            (p / 'seed-manifest.json').write_text(json.dumps(dict(chain=CHAIN, source_romfs_sha256='a')))
            with self.assertRaisesRegex(ValueError, 'RomFS'):
                Seed(p, 'b')

    def test_seed_checks_file_hashes(self):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            data = b'{"value": 1}'
            (p / 'item.json').write_bytes(data)
            manifest = dict(chain=CHAIN, source_romfs_sha256='a',
                            files={'item.json': hashlib.sha256(data).hexdigest()})
            (p / 'seed-manifest.json').write_text(json.dumps(manifest))
            seed = Seed(p, 'a')
            self.assertEqual(seed.checked_json('item.json'), {'value': 1})
            (p / 'item.json').write_bytes(data + b' ')
            with self.assertRaisesRegex(ValueError, 'integrity'):
                seed.checked_json('item.json')
            with self.assertRaisesRegex(ValueError, 'unsafe'):
                seed.checked_json('../item.json')


if __name__ == '__main__':
    unittest.main()
