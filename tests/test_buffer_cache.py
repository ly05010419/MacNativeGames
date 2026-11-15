"""Bounded buffer reuse must preserve in-flight data and retirement fences."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
from dev.game_slot import GameSlot


@unittest.skipUnless(sys.platform == 'darwin', 'requires Metal 4')
class BufferCache(unittest.TestCase):
    def test_gpu_fences_and_readback_bounds(self):
        source = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_buffer_cache.h'
        fixed = source.read_text()
        negative = fixed.replace('entry->fence > done || ', '')
        self.assertNotEqual(fixed, negative, 'negative control must remove the fence guard')
        template = (ROOT / 'tests/buffer_cache_probe.m').read_text()
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            binaries = []
            for name, text in [('fixed', fixed), ('early-reuse', negative)]:
                header, file, binary = work / (name + '.h'), work / (name + '.m'), work / name
                header.write_text(text)
                file.write_text(template.replace('"../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_buffer_cache.h"', '"' + str(header) + '"'))
                subprocess.run(['xcrun', 'clang', '-O1', '-fobjc-arc', '-fsanitize=address,undefined',
                                str(file), '-framework', 'Foundation', '-framework', 'Metal', '-o', str(binary)], check=True)
                binaries.append((name, binary))
            with GameSlot(owner='tests', purpose='buffer cache GPU fences and memory bounds', expect_seconds=60) as slot:
                for name, binary in binaries:
                    process = slot.popen([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    try:
                        out, err = process.communicate(timeout=20)
                    finally:
                        if process.poll() is None:
                            process.kill(); process.wait(timeout=5)
                    if process.returncode == 77:
                        self.skipTest('Metal 4 unavailable')
                    if name == 'fixed':
                        self.assertEqual(process.returncode, 0, out + err)
                        self.assertIn('16-buffer/16-MiB readback bounds checked', out)
                    else:
                        self.assertEqual(process.returncode, 1, out + err)
                        self.assertIn('an in-flight table must not be reused', err)
                    print(name, out.strip(), err.strip())


if __name__ == '__main__':
    unittest.main()
