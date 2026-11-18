"""Packaging prunes the pipeline archive to what the lists still have (totk_pipeline_archive_files.h par_prune_superseded,
2026-11-13): a recheck run (TOTK_PAR_RECHECK=1, build_app.mac_pipelines and device_pipelines.mac_cache) records the keys
it used (g_par_used) and removes the files holding none of them. Before, a translator change that gave about half of
Ocarina of Time's shaders new keys left every old file in place (0 removed), and the iPad's device archive was compiled
with them."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
PROBE = ROOT / 'tests/archive_prune_probe.m'


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class ArchivePrune(unittest.TestCase):
    def test_prune_on_this_mac(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'probe'
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-w', f'-I{NVN}', str(PROBE), '-framework', 'Foundation',
                                    '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        lines = r.stdout.splitlines()
        self.assertTrue(lines and lines[-1].startswith('done '), r.stdout[-2000:] + r.stderr[-2000:])
        self.assertEqual([l for l in lines if l.startswith('FAIL')], [])
        self.assertEqual(lines[-1], 'done 0')

    def test_every_lookup_and_write_records_its_key(self):
        """Every pipeline the packaging run makes goes through par_archive_for, and a new one through par_write: both
        record the key while rechecking, or its file would be pruned as unlisted."""
        text = (NVN / 'totk_pipeline_archive.h').read_text()
        record = 'if (g_par_recheck) { if (!g_par_used) g_par_used = [NSMutableSet set]; [g_par_used addObject:key]; }'
        for fn in ('static id<MTLBinaryArchive> par_archive_for(id<MTLDevice> dev, NSString *key) {',
                   'static void par_write(id<MTLDevice> dev, NSString *key, id desc) {'):
            body = text[text.index(fn):]
            body = body[:body.index('\n}\n')]
            self.assertIn(record, body, fn)


if __name__ == '__main__':
    unittest.main()
