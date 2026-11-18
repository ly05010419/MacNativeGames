"""A completed readback must not escape into a long-lived caller's outer pool."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
from dev.game_slot import GameSlot


@unittest.skipUnless(sys.platform == 'darwin', 'requires Metal 4')
class FeedbackLifetime(unittest.TestCase):
    def test_production_callback_drains_temporary_receivers(self):
        source = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        start = source.index('@interface TotkCommitOptions')
        end = source.index('\n@end', source.index('@implementation TotkCommitOptions', start)) + len('\n@end')
        fixed = source[start:end]
        negative = re.sub(r'@autoreleasepool\s*\{\s*block\(feedback\);\s*\}', 'block(feedback);', fixed)
        self.assertNotEqual(fixed, negative, 'negative control must remove the production pool')
        template = (ROOT / 'tests/feedback_lifetime_probe.m').read_text()
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            binaries = []
            for name, body in [('fixed', fixed), ('unbounded-caller-pool', negative)]:
                file, binary = work / (name + '.m'), work / name
                file.write_text(template.replace('// COMMIT_OPTIONS', body))
                subprocess.run(['xcrun', 'clang', '-O1', '-fobjc-arc', '-fsanitize=address,undefined',
                                str(file), '-framework', 'Foundation', '-framework', 'Metal', '-o', str(binary)], check=True)
                binaries.append((name, binary))
            with GameSlot(owner='tests', purpose='GPU completion temporary lifetime', expect_seconds=60) as slot:
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
                        self.assertIn('readback destroyed before caller pool drains', out)
                    else:
                        self.assertEqual(process.returncode, 1, out + err)
                        self.assertIn("temporary receiver must not escape", err)
                    print(name, out.strip(), err.strip())


if __name__ == '__main__':
    unittest.main()
