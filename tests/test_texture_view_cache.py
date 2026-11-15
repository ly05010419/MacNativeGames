"""Exercise the production sampling-view cache on an uploaded texture owner."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
from dev.game_slot import GameSlot


@unittest.skipUnless(sys.platform == 'darwin', 'requires Metal on Apple platforms')
class TextureViewCache(unittest.TestCase):
    def test_owner_and_full_view_key(self):
        source = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        start = source.index('static void pkg_views_drop(')
        drop = source[start:source.index('\n}', start) + 2]
        start = source.index('static uint64_t g_stor_sampling_hits')
        cache = source[start:source.index('\nstatic int stor_key_lookup', start)]
        template = Path(__file__).with_name('texture_view_cache_probe.m').read_text()
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            binaries = []
            for name, code in [('fixed', cache), ('uncached-package', cache.replace('NvPkg *pkg = candidate && candidate->tex == base ? candidate : nil;', 'NvPkg *pkg = nil;'))]:
                file, binary = work / (name + '.m'), work / name
                file.write_text(template.replace('// PRODUCTION', drop + '\n' + code))
                subprocess.run(['xcrun', 'clang', '-O1', '-fobjc-arc', '-fsanitize=address,undefined', str(file),
                                '-framework', 'Foundation', '-framework', 'Metal', '-o', str(binary)], check=True)
                binaries.append((name, binary))
            with GameSlot(owner='tests', purpose='owned texture view cache', expect_seconds=30) as slot:
                for name, binary in binaries:
                    p = slot.popen([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    try:
                        out, err = p.communicate(timeout=15)
                    finally:
                        if p.poll() is None:
                            p.kill(); p.wait(timeout=5)
                    if p.returncode == 77:
                        self.skipTest('requires Metal')
                    self.assertEqual(p.returncode, 0 if name == 'fixed' else 1, out + err)
                    self.assertIn('PASS package views' if name == 'fixed' else 'same base/layout returns same GPU view', out + err)
                    print(name, out.strip(), err.strip())


if __name__ == '__main__':
    unittest.main()
