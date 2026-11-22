"""Breath of the Wild's sync lag and the storage-buffer copies that keep it correct (PR #2201, #2338; 2026-11-22).

Why it is this way -- read before changing it (user 2026-11-22: "以后不要又改回去了 如果改回去 必须解决现在这个问题"):

- TOTK_SYNC_LAG (on by default in Breath of the Wild) settles the game's wait for the frame it just submitted for the one
  before. The game then refills read-only storage buffers the GPU has still to read: the Game Over title and cutscene
  name cards showed another frame's glyphs (text shader 21fe759b). OOT_RACE_CHECK on the desktop BotW 1.9 app counted
  12,909 such races in 11,400 submissions.
- OOT_SBUF_SNAPSHOT copies those buffers at the encode, so the GPU reads the bytes the game submitted. Breath of the
  Wild has it on by default: read-only storage races 12,909 -> 0, frame rate unchanged (136.2 against 135.2 fps,
  p95 9.67 against 9.75 ms, same save, 100 s standing, M4).
- The user chose the frame rate: honouring the wait instead (TOTK_SYNC_LAG=0) cost 18% (110.5 fps); a per-page read
  fence (OOT_GPU_READ_FENCE=1 / =2 / =2 with kinds v and w) 8-23%. So the sync lag stays and the copy stays.
- Tears of the Kingdom and the other games keep the copy off (their wait is not relaxed); measured on Lookout Landing:
  12.63 against 12.56 ms, GPU 9.94 both.

If a test here fails because someone turned something back, do not just update the test: solve the problem first, as
each failure message says, and record the new measurements here and in issues_lessons.md.
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
        self.assertRegex(MTL, r'#ifdef GAME_BOTW\n#define SBUF_SNAP_DEFAULT 1\n#else\n#define SBUF_SNAP_DEFAULT 0\n#endif\n',
            'Breath of the Wild must copy small read-only storage buffers at the encode by default while its sync lag is on: '
            'without the copy the Game Over / name-card glyphs show another frame (PR #2201, 12,909 races). Turning it off '
            'needs the wait made honest first (TOTK_SYNC_LAG off by default or a fence that keeps the frame rate) and '
            'OOT_RACE_CHECK=1 showing s=0 races on the same save; record the numbers in this file and in issues_lessons.md.')
        self.assertIn('on = e ? atoi(e) : SBUF_SNAP_DEFAULT;', MTL,
            'sbuf_snap_on must take SBUF_SNAP_DEFAULT when OOT_SBUF_SNAPSHOT is not set (Breath of the Wild: on).')

    def test_the_copies_are_there_because_breath_of_the_wild_keeps_the_sync_lag(self):
        self.assertRegex(HLE, r'#ifdef GAME_BOTW\n#define SYNC_LAG_DEFAULT 1\n',
            'Breath of the Wild keeps TOTK_SYNC_LAG on by default for the frame rate (user 2026-11-22: off cost 18%, '
            '135 -> 110.5 fps on the M4). If it goes off by default, measure the frame rate on the fixtures '
            '(botw160-complete-hebra, botw160-play-1028, run2.sh) and let the user decide; only then may the storage copy '
            'be reconsidered, and this test with it.')

    def test_a_copy_is_not_reused_after_a_gpu_write_to_its_bytes(self):
        msg = ('PR #2338: a shader write whose wait already completed leaves no pending entry, so usnap_drop must drop the '
               'copies of this submission that the write overlaps; otherwise a later binding reuses the bytes from before '
               'the GPU write.')
        self.assertIn('sbuf_snap_drop(a, n);', function(MTL, 'usnap_drop'), msg)
        drop = function(MTL, 'sbuf_snap_drop')
        self.assertIn('e->sub == g_sub_serial', drop, msg)
        self.assertIn('e->sub = UINT64_MAX', drop, msg)

    def test_the_experiments_stay_off_by_default(self):
        msg = ('Measured 2026-11-22 and not adopted: the read fence (OOT_GPU_READ_FENCE) cost 8-23% of the frame rate, the '
               'vertex copy (OOT_VBUF_SNAPSHOT) left most vertex races (they overlap GPU / command-stream writes); the race '
               'check is a diagnostic. Turning one on by default needs new measurements and the user\'s decision.')
        self.assertIn('const char *e = getenv("OOT_VBUF_SNAPSHOT"); v = e ? atoi(e) : 0;', MTL, msg)
        self.assertIn('const char *e = getenv("OOT_GPU_READ_FENCE"); on = e && atoi(e);', MTL, msg)
        self.assertIn('const char *e = getenv("OOT_RACE_CHECK"); on = e && atoi(e);', MTL, msg)

    def test_the_vertex_copy_never_replaces_data_the_gpu_writes(self):
        body = MTL[MTL.index('static uint64_t vsnap_get('):]
        body = body[:body.index('\n}')]
        self.assertLess(body.index('pool_range_write_exact(&u, &over)'), body.index('memcpy(cpu'),
            'OOT_VBUF_SNAPSHOT must check for pending GPU / command-stream writes before it copies: a CPU copy must never '
            'replace data the GPU produces.')

    def test_the_reason_is_written_next_to_the_switch(self):
        self.assertIn('DO NOT TURN THIS OFF for Breath of the Wild while its TOTK_SYNC_LAG stays on', MTL,
            'Keep the note next to SBUF_SNAP_DEFAULT in nvn_mtl.m: why the copy is on and what must be solved before it '
            'goes off.')


if __name__ == '__main__':
    unittest.main()
