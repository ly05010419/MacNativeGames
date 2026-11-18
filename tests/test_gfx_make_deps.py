"""The Makefiles that build the shared graphics layer rebuild an object when a file it includes changes, .inc files too.
2026-11-13: games/totk/Makefile listed only *.h in GFX_HEADERS; a change to nvn_queue_order.inc (included by hle_nvn.c)
left the old hle_nvn.c.o in place, and an express update shipped a runtime without it while reporting the new commit."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAYER = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


class GfxMakeDeps(unittest.TestCase):
    def test_every_makefile_of_the_shared_layer_depends_on_its_inc_files(self):
        self.assertTrue(list(LAYER.glob('*.inc')), 'the shared layer has .inc files')
        for game in ('totk', 'oot'):
            with self.subTest(game=game):
                make = (ROOT / 'games' / game / 'Makefile').read_text()
                headers = re.search(r'^GFX_HEADERS\s*:=\s*(.+)$', make, re.M)
                self.assertIsNotNone(headers, f'games/{game}/Makefile has no GFX_HEADERS')
                self.assertIn('third_party/d2r_gfx/gfx/nvn/*.inc', headers.group(1))


if __name__ == '__main__':
    unittest.main()
