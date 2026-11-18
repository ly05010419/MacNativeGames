"""A pool made or removed must not make the replay do again what does not depend on that pool.

2026-11-18, Ocarina of Time: about 280 frames into the world the game makes 220 memory pools in six frames, on other
threads while the game thread replays its command lists. Every pool changed the global pool generation, and two things
were keyed on it: the captures of the handle table (the whole 5 MB table copied and scanned again, 38 times or 198 MB in
one frame) and the raw pool table (built again 43 times, 30 ms). That frame's replay took 75-131 ms instead of 15: the
one frame over 100 ms left on the user's route. A capture depends on its own table's dirty range, and the raw table on
the pools a command list can name.

The Metal probe needs MNG_OOT_PROBE, MNG_OOT_GAME_DATA and MNG_OOT_REWRITTEN (an externally built oot-native and the
player's verified data). No game data lives in tests.
"""
import json
import os
from pathlib import Path
import subprocess
import unittest


NVN = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn'
PROBE = 'probe-gpu-memory-demand'


class WhatEachDependsOn(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mtl = (NVN / 'nvn_mtl.m').read_text()
        cls.dirty = (NVN / 'nvn_dirty.c').read_text()

    def test_a_range_registered_again_is_another_epoch(self):
        self.assertIn('uint64_t nvn_dirty_span_epoch(const void *addr, size_t len);', (NVN / 'nvn_dirty.h').read_text())
        # both builds of the tracker (the BotW one keeps several ranges a pool) number a range when it is registered
        self.assertEqual(self.dirty.count('atomic_store(&r->birth, atomic_fetch_add(&g_births, 1) + 1);'), 2)
        self.assertEqual(self.dirty.count('uint64_t nvn_dirty_span_epoch(const void *addr, size_t len) {'), 2)

    def test_captures_follow_their_own_table(self):
        self.assertIn('return nvn_dirty_span_epoch((const void *)(uintptr_t)u->addr,(size_t)u->size);', self.mtl)
        self.assertIn('if(p->epoch==pool_input_epoch(u)&&p->hostwrite==', self.mtl, 'a capture found again')
        self.assertIn('b->epoch==epoch&&b->hostwrite==hostwrite;', self.mtl, 'the incremental copy')
        self.assertIn('if(m&&m->epoch==p->epoch&&m->hostwrite==p->hostwrite&&m->stamp<=p->stamp&&exact&&m->shadow&&m->counts){',
                      self.mtl, 'the ID set')
        self.assertNotIn('poolgen', self.mtl, 'nothing of a capture is keyed on the global pool generation')

    def test_the_id_set_is_exact(self):
        """Without the restarts the pool generation used to bring, a union only grows: 11.8 million descriptor entries
        prepared in 300 frames instead of 0.4 million, 40.5 fps instead of 43.9 (fix-1)."""
        ids = self.mtl[self.mtl.index('static void pool_input_ids(pool_input *p,const p_bind_buf *u,int n) {'):]
        ids = ids[:ids.index('/* OOT (2026-11-08): captures of a handle table reuse dedicated buffers')]
        self.assertIn('if(old<(uint32_t)n&&!--count[old]){', ids, 'an ID leaves with the last word that names it')
        self.assertIn('if(now<(uint32_t)n&&!count[now]++){', ids)
        self.assertIn('if(changed)m->ids=changed;', ids, 'a changed set is a new object: earlier captures keep theirs')
        self.assertEqual(ids.count('changed=[m->ids mutableCopy];bits=changed.mutableBytes;'), 2)
        self.assertIn('if(nvn_dirty_runs_since((const void *)(uintptr_t)u->addr,(size_t)u->size,m->stamp,pool_ids_note,&r)&&!r.over){',
                      ids, 'more written runs than the list holds count the table again')

    def test_the_raw_table_follows_the_list_being_replayed(self):
        self.assertIn('if (g_exec_depth++ == 0) g_exec_pool_gen = atomic_load_explicit(&g_pool_gen, memory_order_acquire);',
                      self.mtl)
        self.assertIn('if (--g_exec_depth == 0) g_exec_pool_gen = UINT64_MAX;', self.mtl)
        self.assertIn('need = MAX(g_exec_pool_gen, drop); }', self.mtl)
        self.assertIn('if (g_gmem_buf[cached] && g_gmem_gen[cached] >= need && g_gmem_index_mode[cached] == indexed)',
                      self.mtl)

    def test_what_takes_a_pool_away_is_recorded(self):
        # a pool removed, an issued address forgotten, a pool registered over another's range
        self.assertEqual(self.mtl.count('atomic_store_explicit(&g_pool_drop_gen'), 3)
        add = self.mtl[self.mtl.index('int over = pidx_overlaps(&g_pools, lo, hi);'):]
        self.assertIn('if (over) atomic_store_explicit(&g_pool_drop_gen, gen, memory_order_release);', add[:1200])

    def test_one_residency_commit_a_build(self):
        build = self.mtl[self.mtl.index('static uint64_t gmem_map_gpu(int compatible) {'):]
        build = build[:build.index('return b.gpuAddress;')]
        self.assertNotIn('pool_res(p, 3);', build, 'that was one kernel call for every pool new to the table')
        self.assertIn('if(made_resident)res_commit();', build)

    def test_the_per_frame_record_has_the_counts(self):
        self.assertIn('{"@pool_gen", "@pool_adds", "@cap_whole", "@cap_whole_mb", "@ids_full", "@gmem_builds", '
                      '"@gmem_build_ms", "@cbc_miss", "@dpool_miss"}', (NVN / 'hle_nvn.c').read_text())
        self.assertIn('void mtl_pool_gen_stats(uint64_t out[9]) {', self.mtl)

    def test_the_replay_profile_is_off_unless_asked(self):
        order = (NVN / 'nvn_queue_order.inc').read_text()
        self.assertIn('const char *e = getenv("TOTK_REPLAY_PROFILE_MS"); v = e && atoi(e) > 0 ?', order)
        self.assertIn('if (qo_profile_ns()) {', order)


@unittest.skipUnless(all(os.environ.get(k) for k in ('MNG_OOT_PROBE', 'MNG_OOT_GAME_DATA', 'MNG_OOT_REWRITTEN')),
                     'requires the built OOT Metal runtime and verified local data')
class PoolSetProbe(unittest.TestCase):
    CHECKED = 379   # 367 before 2026-11-18; 4 for the exact ID set and 3 for the captures (pool_ids_probe), 5 for the raw table (gmem_list_gen_probe)

    def run_probe(self, **switches):
        env = dict(os.environ, **switches, TOTK_GAME_DATA=os.environ['MNG_OOT_GAME_DATA'], TOTK_SLOT_OWNER='claude',
                   TOTK_SLOT_PURPOSE='pool-set-changes-regression', TOTK_SLOT_EXPECT_S='60')
        result = subprocess.run([os.environ['MNG_OOT_PROBE'], os.environ['MNG_OOT_REWRITTEN'], PROBE],
                                env=env, capture_output=True, text=True, timeout=120)
        return result, result.stdout + result.stderr

    def test_other_pools_leave_captures_and_the_list_s_table_alone(self):
        result, log = self.run_probe()
        self.assertEqual(result.returncode, 0, log[-10000:])
        records = [json.loads(line) for line in log.splitlines() if line.startswith('{"status":')]
        self.assertTrue(records, log[-4000:])
        self.assertEqual((records[-1]['status'], records[-1]['probe']), ('pass', PROBE))
        self.assertEqual(records[-1]['checked'], self.CHECKED)

    def test_captures_keyed_on_the_global_generation_fail(self):
        """Negative control: as before, a pool made elsewhere captures the table again."""
        result, log = self.run_probe(OOT_POOL_INPUT_EPOCH='0')
        self.assertNotEqual(result.returncode, 0, log[-4000:])
        self.assertIn('pool ids: a pool made elsewhere captured the table again', log)

    def test_a_union_of_ids_fails(self):
        """Negative control: as before, an ID no table word names any more stays in the set."""
        result, log = self.run_probe(OOT_POOL_IDS_EXACT='0')
        self.assertNotEqual(result.returncode, 0, log[-4000:])
        self.assertIn('pool ids: an ID no longer named stayed in the set', log)

    def test_a_table_built_after_every_change_fails(self):
        """Negative control: as before, a pool made while a list is replayed builds the raw table again."""
        result, log = self.run_probe(OOT_GMEM_LIST_GEN='0')
        self.assertNotEqual(result.returncode, 0, log[-4000:])
        self.assertIn('raw table: a pool made while a list is replayed built the table again', log)


if __name__ == '__main__':
    unittest.main()
