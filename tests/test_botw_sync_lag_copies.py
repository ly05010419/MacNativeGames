"""Breath of the Wild's sync lag and the storage-buffer copies that keep it correct (PR #2201, #2338; 2026-11-22).

TOTK_SYNC_LAG (on by default in Breath of the Wild) settles the game's wait for the frame it just submitted for the one
before, so the game refills read-only storage buffers the GPU has still to read (the Game Over glyphs, text shader
21fe759b). OOT_SBUF_SNAPSHOT copies them at the encode; Breath of the Wild has it on by default. Measured on the desktop
BotW 1.9 app: read-only storage races 12,909 -> 0, 135.1 against 135.2 fps. The experiments measured the same night
(OOT_VBUF_SNAPSHOT, OOT_GPU_READ_FENCE) stay off by default: the read fence cost 8-23% of the frame rate.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
MTL = (NVN / 'nvn_mtl.m').read_text()
HLE = (NVN / 'hle_nvn.c').read_text()


def function(source, name):   # the definition, not a forward declaration
    start = re.search(rf'static void {name}\([^)]*\) {{', source).start()
    return source[start:source.index('\n}', start)]


class BotwSyncLagCopies(unittest.TestCase):
    def test_storage_copies_are_on_by_default_in_breath_of_the_wild_only(self):
        self.assertRegex(MTL, r'#ifdef GAME_BOTW\n#define SBUF_SNAP_DEFAULT 1\n#else\n#define SBUF_SNAP_DEFAULT 0\n#endif\n')
        self.assertIn('on = e ? atoi(e) : SBUF_SNAP_DEFAULT;', MTL)

    def test_the_copies_are_there_because_breath_of_the_wild_keeps_the_sync_lag(self):
        # If the sync lag ever goes off by default, the copies are no longer needed for this reason: measure again.
        self.assertRegex(HLE, r'#ifdef GAME_BOTW\n#define SYNC_LAG_DEFAULT 1\n')

    def test_a_copy_is_not_reused_after_a_gpu_write_to_its_bytes(self):
        # PR #2338: a shader write whose wait already completed leaves no pending entry; usnap_drop records it.
        self.assertIn('sbuf_snap_drop(a, n);', function(MTL, 'usnap_drop'))
        drop = function(MTL, 'sbuf_snap_drop')
        self.assertIn('e->sub == g_sub_serial', drop)
        self.assertIn('e->sub = UINT64_MAX', drop)

    def test_the_experiments_stay_off_by_default(self):
        self.assertIn('const char *e = getenv("OOT_VBUF_SNAPSHOT"); v = e ? atoi(e) : 0;', MTL)
        self.assertIn('const char *e = getenv("OOT_GPU_READ_FENCE"); on = e && atoi(e);', MTL)
        self.assertIn('const char *e = getenv("OOT_RACE_CHECK"); on = e && atoi(e);', MTL)

    def test_the_vertex_copy_never_replaces_data_the_gpu_writes(self):
        body = MTL[MTL.index('static uint64_t vsnap_get('):]
        body = body[:body.index('\n}')]
        self.assertLess(body.index('pool_range_write_exact(&u, &over)'), body.index('memcpy(cpu'))


if __name__ == '__main__':
    unittest.main()
