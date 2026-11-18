"""Real Metal heap reuse, parent-view lifetime and idle destruction."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
from dev.game_slot import GameSlot


@unittest.skipUnless(sys.platform == 'darwin', 'requires Metal on Apple platforms')
class HeapCache(unittest.TestCase):
    def test_lifetime_and_gpu_data(self):
        header = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_heap_cache.h'
        text = header.read_text()
        negative = text.replace('id<MTLHeap> h = rt_heap_take(d, need.size, 0);', 'id<MTLHeap> h = nil;')
        self.assertNotEqual(text, negative)
        template = Path(__file__).with_name('heap_cache_probe.m').read_text()
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            binaries = []
            for name, source in [('fixed', text), ('no-reuse', negative)]:
                local, file, binary = work / (name + '.h'), work / (name + '.m'), work / name
                local.write_text(source)
                file.write_text(template.replace('"../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_heap_cache.h"', '"' + str(local) + '"'))
                subprocess.run(['xcrun', 'clang', '-O1', '-fobjc-arc', '-fsanitize=address,undefined', str(file),
                                '-framework', 'Foundation', '-framework', 'Metal', '-o', str(binary)], check=True)
                binaries.append((name, binary))
            with GameSlot(owner='tests', purpose='heap recycling lifetime and mip/layer pixels', expect_seconds=60) as slot:
                for name, binary in binaries:
                    p = slot.popen([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    try:
                        out, err = p.communicate(timeout=20)
                    finally:
                        if p.poll() is None:
                            p.kill(); p.wait(timeout=5)
                    if p.returncode == 77:
                        self.skipTest('requires Metal 4')
                    self.assertEqual(p.returncode, 0 if name == 'fixed' else 1, out + err)
                    self.assertIn('PASS heap reuse' if name == 'fixed' else 'retired heap is reused', out + err)
                    print(name, out.strip(), err.strip())


if __name__ == '__main__':
    unittest.main()
