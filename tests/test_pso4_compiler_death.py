"""The Metal 4 pipeline archive drops every unwritten period begun before a compiler service death (nvn_pso4.h, after
nxnative 9bd8bb38): a period written after the service died holds pipelines whose functions are missing, and the next
start aborts in Metal's lookup of that file. Source checks: every compile failure path reports the death, the period
records the death count it began under, and its writer drops it instead of writing or splitting it. A game run with
TOTK_PSO4_TEST_DEATH=<n> shows the drop in the log."""
from pathlib import Path
import re
import unittest

NVN = Path(__file__).resolve().parents[1]/'games/totk/third_party/d2r_gfx/gfx/nvn'
PSO4 = (NVN/'nvn_pso4.h').read_text(encoding='utf-8')
BUILD = (NVN/'nvn_pso4_build.h').read_text(encoding='utf-8')
MTL = (NVN/'nvn_mtl.m').read_text(encoding='utf-8')
MESH = (NVN/'nvn_mesh_tasks.inc').read_text(encoding='utf-8')


def body(source, signature):
    start = source.index(signature)
    depth, i = 0, source.index('{', start)
    for j in range(i, len(source)):
        depth += {'{': 1, '}': -1}.get(source[j], 0)
        if not depth:
            return source[i:j+1]
    raise AssertionError(signature)


class CompilerDeath(unittest.TestCase):
    def test_the_death_is_recognised_by_metals_error_text(self):
        died = body(PSO4, 'static int par4_err_died(NSError *e) {')
        self.assertIn('XPC_ERROR_CONNECTION_INTERRUPTED', died)
        self.assertIn('interrupted connection', died)

    def test_a_period_begun_before_a_death_is_not_written(self):
        self.assertIn('p->deaths = atomic_load(&g_par4_deaths);', body(PSO4, 'static Par4Period *par4_new_period('))
        write = body(PSO4, 'static void par4_write(Par4Period *p) {')
        drop = write.index('if (atomic_load(&g_par4_deaths) != p->deaths)')
        self.assertLess(write.index('dispatch_group_wait(p->busy'), drop)   # after its compiles return
        self.assertLess(drop, write.index('par4_publish('))                  # before anything is written
        self.assertLess(drop, write.index('par4_resplit('))                  # and never split into new files
        died = body(PSO4, 'static void par4_compiler_died(NSError *e) {')
        self.assertIn('atomic_fetch_add(&g_par4_deaths, 1)', died)
        self.assertIn('g_par4_period = nil;', died)   # later captures go to a new period

    def test_every_compile_failure_reports_the_death(self):
        make = body(BUILD, 'static id par4_make(int kind, id desc4, NSString *key)')
        self.assertIn('if (par4_err_died(ce)) par4_compiler_died(ce); else par4_refused(p);', make)
        self.assertIn('else if (!ps) par4_compiler_died(ce);', make)          # a plain compile
        self.assertIn('if (!ps && ce && p) par4_compiler_died(ce);', make)   # the retry without capture
        self.assertEqual(MTL.count('par4_compiler_died(e ? *e : nil);'), 2)  # pso_new and cs_new_ on the MTLDevice path
        self.assertIn('par4_compiler_died(error?*error:nil);', MESH)         # mesh pipelines (old archive)
        self.assertEqual(len(re.findall(r'gpu_compile_error\(', MTL + MESH)), 3)   # no other compile failure sink

    def test_a_failed_serialization_counts_as_a_death_when_the_service_died(self):
        write = body(PSO4, 'static void par4_write(Par4Period *p) {')
        self.assertLess(write.index('par4_compiler_died(why);'), write.index('par4_resplit(keys, descs);'))


if __name__ == '__main__':
    unittest.main()
