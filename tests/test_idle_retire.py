"""Production completion retirement must make progress with no new submissions."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
from dev.game_slot import GameSlot


@unittest.skipUnless(sys.platform == 'darwin', 'requires Metal on Apple platforms')
class IdleRetire(unittest.TestCase):
    def test_completed_resources_make_idle_progress(self):
        source = (NVN / 'nvn_mtl.m').read_text()
        start = source.index('static NSMutableArray<NvDeferredRelease *> *g_deferred;')
        queue = source[start:source.index('static int g_upstream_negative;', start)]
        start = source.index('static void defer_drain(void) {')
        drain = source[start:source.index('\n#include "nvn_heap_cache.h"', start)].replace('static void defer_drain(void)', 'static void defer_drain_impl(void)', 1)
        start = source.index('static _Atomic int g_retire_idle_scheduled;')
        worker = source[start:source.index('\n#include "nvn_buffer_cache.h"', start)]
        hle = (NVN / 'hle_nvn.c').read_text()
        start = hle.index('int nvn_replay_idle_try(void)')
        replay = hle[start:hle.index('static _Thread_local int t_on_x_thread', start)]
        template = Path(__file__).with_name('idle_retire_probe.m').read_text().replace('// REPLAY_LOCK', replay).replace('// DEFER_QUEUE', queue).replace('// DEFER_DRAIN', drain)
        negative = worker.replace('if (retry) retire_idle_kick();', '(void)retry;')
        self.assertNotEqual(worker, negative)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            binaries = []
            for name, code in [('fixed', worker), ('no-retry', negative), ('future-child', worker.replace('t_retire_cut = g_fence + 1;', '(void)g_fence;'))]:
                file, binary = work / (name + '.m'), work / name
                file.write_text(template.replace('// IDLE_WORKER', code))
                subprocess.run(['xcrun', 'clang', '-O1', '-fobjc-arc', '-fsanitize=address,undefined', f'-I{NVN}', str(file),
                                '-framework', 'Foundation', '-framework', 'Metal', '-o', str(binary)], check=True)
                binaries.append((name, binary))
            with GameSlot(owner='tests', purpose='idle retirement GPU lifetime', expect_seconds=30) as slot:
                for name, binary in binaries:
                    p = slot.popen([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    try:
                        out, err = p.communicate(timeout=15)
                    finally:
                        if p.poll() is None:
                            p.kill(); p.wait(timeout=5)
                    if p.returncode == 77:
                        self.skipTest('requires Metal 4')
                    self.assertEqual(p.returncode, 0 if name == 'fixed' else 1, out + err)
                    self.assertIn('PASS idle retirement' if name == 'fixed' else ('idle retry destroys buffer' if name == 'no-retry' else 'idle destructors retire children'), out + err)
                    print(name, out.strip(), err.strip())


if __name__ == '__main__':
    unittest.main()
