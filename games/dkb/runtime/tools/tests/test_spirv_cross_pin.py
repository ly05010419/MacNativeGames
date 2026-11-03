"""The in-repo spirv-cross (dm/gfx/tools/spirv_cross_bin, GitHub issue #242): the shader tools run it rather than the one on PATH, the lock records its fingerprint, it is the
release build.sh pins, it runs on both Mac architectures, and SPIRV-Cross's licences are beside it. No Xcode or game files needed."""
import os
from pathlib import Path
import re
import subprocess
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
BIN = REPO / 'dm/gfx/tools/spirv_cross_bin'
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain  # noqa: E402
import tool_lock  # noqa: E402


class Pin(unittest.TestCase):
    def test_tools_run_the_in_repo_build(self):
        self.assertEqual(Path(dmsh_chain.SPIRV_CROSS), BIN / 'spirv-cross')
        for tool in ('dmsh_build.py', 'dmsh_dryrun.py', 'vk_shaders.py', 'gl_shaders.py'):
            self.assertNotRegex((REPO / 'dm/gfx/tools' / tool).read_text(), r"\[\s*'spirv-cross'", tool)

    def test_lock_records_it(self):
        with open(dmsh_chain.LOCK) as f: lock = dict(l.rstrip('\n').split('\t', 1) for l in f if '\t' in l and not l.startswith('#'))
        self.assertEqual(lock['spirv-cross'], tool_lock.sha16(BIN / 'spirv-cross'))

    def test_pinned_release_and_licences(self):
        m = re.search(r'^TAG=(\S+)', (BIN / 'build.sh').read_text(), re.M)
        assert m, 'build.sh pins no TAG'
        tag = m[1]
        r = subprocess.run([str(BIN / 'spirv-cross'), '--revision'], capture_output=True, text=True)
        self.assertIn(f'Git commit: {tag} ', r.stdout + r.stderr)
        self.assertTrue(os.access(BIN / 'spirv-cross', os.X_OK))
        for f in ('LICENSE', 'LICENSES/Apache-2.0.txt', 'LICENSES/MIT.txt'): self.assertTrue((BIN / f).is_file(), f)

    def test_universal(self):   # an Intel Mac runs it too, as the translator's launcher
        r = subprocess.run(['lipo', '-archs', str(BIN / 'spirv-cross')], capture_output=True, text=True)
        self.assertEqual(sorted(r.stdout.split()), ['arm64', 'x86_64'])


if __name__ == '__main__':
    unittest.main()
