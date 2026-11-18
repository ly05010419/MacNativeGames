"""Ocarina of Time on the Mac drops texture copies no submission used for 6,000 submissions (mode 3 of pkg_evict_sweep,
nvn_mtl.m; 2026-11-17, target 12 GB): the default is OOT + Mac only, TOTK_TEX_EVICT_IDLE overrides it (0 = off), and
the other games and iOS keep 0 (their own rules)."""
from pathlib import Path
import re
import unittest

SRC = (Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()


class CopyEvictIdle(unittest.TestCase):
    def sweep(self):
        start = SRC.index('static void pkg_evict_sweep(void) {')
        return SRC[start:SRC.index('\n}\n', start)]

    def test_default_is_oot_mac_only(self):
        body = self.sweep()
        m = re.search(r'#if defined\(GAME_OOT\) && !TARGET_OS_IPHONE\s*\n\s*idle_age = a && \*a \? strtoull\(a, NULL, 10\) : (\d+);'
                      r'\s*\n#else\s*\n\s*idle_age = a && \*a \? strtoull\(a, NULL, 10\) : (\d+);\s*\n#endif', body)
        self.assertIsNotNone(m, 'idle_age default block not found')
        self.assertEqual(m.group(1), '6000')
        self.assertEqual(m.group(2), '0')

    def test_mode_three_takes_every_idle_copy(self):
        body = self.sweep()
        self.assertIn('budget ? 2 : idle_age ? 3 : TARGET_OS_IPHONE ? 1 : 0', body)   # an explicit budget still wins
        self.assertIn('mode == 3 ? idle_age :', body)
        self.assertIn('if (mode == 3) want = UINT64_MAX;', body)
        self.assertIn('TOTK_TEX_EVICT_IDLE', body)

    def test_only_mirrored_copies_go(self):
        # the GPU never rendered into them (owned) and they are not content-hashed small textures
        self.assertIn('if (!pk->tex || pk->owned || pk->hashed) continue;', self.sweep())


if __name__ == '__main__':
    unittest.main()
