"""Alias region heaps grow with room, and a migration leaves long-idle layouts behind (nvn_alias_plan.h,
nvn_stor_migrate.h; 2026-11-18, user: "为什么 周围都加载了 着色器和管线也有了 还是不稳定 有这么多长帧").

Measured in Ocarina of Time on the user's recorded route (scratch/runs/2026-11/oot-route): the game lays its render
targets out again every frame in one stretch of a pool; the stretch a format uses only widens as heavier frames come,
by a median of 400 KiB, at either end. Each widening replaced the format's region heap by the exact union and copied
every texture on it: 366 migrations and 192.9 GiB of copies in 55 s, up to 4.2 GiB (2,697 depth textures over 59 MiB
of heap) in one frame, 3-7 frames in a row; 13 of the frames shown 100 ms or longer. About five of every six textures
copied were layouts of earlier frames that no texture object held any more.
A heap's unused bytes are real memory (a 512 MiB placement heap with 1 MiB written: footprint +553 MB), so regions
cannot simply be made large; and regions must not overlap (a first version that started another heap at the texture
made a heap per frame at the seam: 2.9 GB of heaps).

The probe (tests/alias_region_plan_probe.c) checks the growth rule and replays the measured growth."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


class AliasRegionPlan(unittest.TestCase):
    def test_the_rule_and_the_measured_growth(self):
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d) / 'probe'
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(NVN),
                            str(ROOT / 'tests/alias_region_plan_probe.c'), '-o', str(exe)], check=True)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertNotIn('FAILED', run.stdout)
            self.assertEqual(run.stdout.count('ok: '), 22, run.stdout)

    def test_the_backend_makes_regions_by_the_plan(self):
        text = (NVN / 'nvn_stor_alias.h').read_text()
        self.assertIn('#include "nvn_alias_plan.h"', text)
        self.assertIn('alias_region_plan(g_alias_spans, n, g_alias_others, m, lo, hi, stor_alias_room())', text)
        # on unless turned off, for the comparison run
        self.assertIn('const char *e = getenv("OOT_ALIAS_ROOM"); on = e ? atoi(e) != 0 : 1;', text)
        # only the regions the texture touches are replaced: the room stops short of the others
        self.assertIn('r->lo < hi && lo < r->lo + r->size) {\n            old_lo', text)

    def test_a_migration_releases_long_idle_entries(self):
        text = (NVN / 'nvn_stor_migrate.h').read_text()
        self.assertIn('n = e ? atoll(e) : 64;', text, '64 submissions unless OOT_MIGRATE_IDLE says otherwise')
        self.assertIn('if (!detached && !entry->nser && idle_after && g_fence > entry->idle_fence + idle_after) {', text)
        self.assertIn('for (int i = 0; i < nidle; i++) stor_drop_locked(idle[i]);', text)

    def test_a_migration_copies_the_layout_made_last_last(self):
        """2026-11-18 (user: "存档开头 还有 白色 光点 和渲染问题"): the layouts on a heap overlap and every copy writes its
        layout's whole content into the new heap, so the last copy is what the shared bytes hold. They were copied in
        hash order: entering the world, a layout of an earlier frame was copied over this frame's, and two frames showed
        white blocks and black dashes all over the picture (oot-route/room-1/burst/b01-22.png: 87,870 and 147,801
        flashing pixels against under 250 around them; with the migration off, or in this order, none). The game makes
        a texture over memory it has just allocated and then writes it, so the layout it made last owns the bytes."""
        mtl = (NVN / 'nvn_mtl.m').read_text()
        acquire = mtl[mtl.index('static id<MTLTexture> stor_acquire(const tex_t *t, MTLTextureDescriptor *d) {'):mtl.index('static void stor_publish(')]
        self.assertIn('e->acq = ++g_stor_acq_seq;', acquire, 'taking an entry again counts as making it')
        publish = mtl[mtl.index('static void stor_publish('):mtl.index('#include "nvn_stor_migrate.h"')]
        self.assertIn('e->acq = ++g_stor_acq_seq;', publish)
        text = (NVN / 'nvn_stor_migrate.h').read_text()
        self.assertIn('@"made":@(entry->acq)', text)
        self.assertIn('if (by_made) { NSComparisonResult made = [a[@"made"] compare:b[@"made"]]; if (made != NSOrderedSame) return made; }', text)
        self.assertIn('const char *e = getenv("OOT_MIGRATE_ORDER"); by_made = e ? atoi(e) != 0 : 1;', text)

    def test_the_per_frame_record_counts_migrations(self):
        self.assertIn('trow(f, "@mig_mb"', (NVN / 'hle_nvn.c').read_text())
        self.assertIn('OOT_STOR_MIGRATE_LOG', (NVN / 'nvn_stor_migrate.h').read_text())


if __name__ == '__main__':
    unittest.main()
