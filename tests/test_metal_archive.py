"""Real Metal descriptor hits, fresh-process persistence and changed-input misses."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
from dev.game_slot import GameSlot


@unittest.skipUnless(sys.platform == 'darwin', 'Metal requires macOS')
class PipelineArchive(unittest.TestCase):
    def test_descriptor_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            binary, archive = work/'probe', work/'archive'
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-framework', 'Foundation', '-framework', 'Metal', str(ROOT/'tests/metal_archive_probe.m'),
                            '-o', str(binary)], check=True)
            def run(mode):
                with GameSlot(owner='tests', purpose='pipeline archive descriptor regression', expect_seconds=60) as slot:
                    proc = slot.popen([str(binary), str(archive), mode], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    try:
                        out, err = proc.communicate(timeout=60)
                    finally:
                        if proc.poll() is None:
                            proc.kill(); proc.wait()
                    self.assertEqual(proc.returncode, 0, out+err)
                    return out
            self.assertIn('16 checks', run('build'))
            before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in archive.iterdir()}
            self.assertTrue(before)
            self.assertIn('16 checks, 16 hits, 0 misses', run('verify'))
            self.assertIn('16 checks, 16 hits, 0 misses', run('build'))
            self.assertIn('0 checks, 0 hits, 8 misses', run('changed'))
            self.assertEqual(before, {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in archive.iterdir()})

    def test_backend_copies_match(self):
        self.assertEqual((ROOT/'games/worldend/runtime/gfx/nvn/mng_pipeline_archive.h').read_bytes(),
                         (ROOT/'games/dread/third_party/d2r_gfx/gfx/nvn/mng_pipeline_archive.h').read_bytes())


if __name__ == '__main__':
    unittest.main()
