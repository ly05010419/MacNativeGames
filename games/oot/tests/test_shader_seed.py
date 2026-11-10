"""A successful exit or a filtered queue is not evidence of a complete seed."""
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import prepare_ns2_seed as seed


class SeedTests(unittest.TestCase):
    def done(self, ok, total):
        return re.search(r'(\d+) of (\d+)', f'{ok} of {total}')

    def test_skipped_mesh_stage_is_incomplete(self):
        inventory = {'a': {'stage': 0}, 'b': {'stage': 4}}
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            seed.require_complete(inventory, ['a'], self.done(1, 1))
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            seed.require_complete(inventory, ['a', 'b'], self.done(1, 2))
        seed.require_complete(inventory, ['a', 'b'], self.done(2, 2))

    def test_wrong_chain_cannot_enter_seed(self):
        with tempfile.TemporaryDirectory() as d:
            folder = Path(d)
            key, suffix = 'a' * 64, 'b' * 12
            variant = key + '-' + suffix
            (folder / (key + '.variants')).write_text(json.dumps([{'variant': variant, 'deps': {}}]))
            (folder / (variant + '_b.metal')).write_text('kernel void main0() {}')
            reflection = folder / (variant + '_b.json')
            reflection.write_text(json.dumps(dict(chain='obsolete', entry='main0', key=variant + '_b')))
            with self.assertRaisesRegex(ValueError, 'reflection'):
                seed.seed_files(folder)
            reflection.write_text(json.dumps(dict(chain=seed.CHAIN, entry='main0', key=variant + '_b')))
            self.assertEqual(len(seed.seed_files(folder)), 3)


if __name__ == '__main__':
    unittest.main()
