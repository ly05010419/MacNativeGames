"""A new device cache folder (a new translation chain or context) takes over this device's pipeline archive and collected
list from the last one (user 2026-11-16: "12 做").

The launchers name the cache rt-<chain>[-<context>], so each new chain began with an empty folder: the device's own
pipeline archive (on this Mac 1,001 pipelines in 311 files, 634 MB) and its pso_collected.tsv were left behind and every
pipeline met in play was compiled again. The archive is good across chains (Metal checks each pipeline's functions,
8c09c8e3); the translations stay per folder. This compiles totk_rt_carry_over out of totk_rt_carry.h and runs it on
folders it makes."""
from pathlib import Path
import os
import subprocess
import tempfile
import time
import unittest

NVN = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn'

PROGRAM = r'''#import <Foundation/Foundation.h>
%s
int main(int argc, char **argv) { @autoreleasepool {
    for (NSString *from in totk_rt_carry_over(@(argv[1]))) printf("moved %%s\n", from.UTF8String);
    return 0;
} }
'''


class CarryOver(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.cache = self.base / 'shader-cache'
        source = self.base / 'carry.m'
        source.write_text(PROGRAM % (NVN / 'totk_rt_carry.h').read_text())
        self.exe = self.base / 'carry'
        built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-framework', 'Foundation', str(source), '-o', str(self.exe)],
                               capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr)

    def tearDown(self):
        self.temp.cleanup()

    def folder(self, name, files=None, collected=None, age=0):
        d = self.cache / name
        d.mkdir(parents=True)
        (d / 'old-translation_b.metal').write_text('kept per folder')
        if files is not None:
            (d / 'pipelines').mkdir()
            for f in files:
                (d / 'pipelines' / f).write_text(name)
            os.utime(d / 'pipelines', (time.time() - age, time.time() - age))
        if collected is not None:
            (d / 'pso_collected.tsv').write_text(collected)
            os.utime(d / 'pso_collected.tsv', (time.time() - age, time.time() - age))
        return d

    def run_carry(self, d):
        return subprocess.run([str(self.exe), str(d)], capture_output=True, text=True, check=True).stdout.splitlines()

    def test_the_newest_archive_and_list_come_to_the_new_folder(self):
        self.folder('rt-v27-aaa', ['a.metalarchive'], 'R\told\n', age=3000)
        self.folder('rt-v28-bbb', ['b.metalarchive', 'b.keys'], 'R\tnewer\n', age=100)
        new = self.cache / 'rt-v29-ccc'; new.mkdir()
        moved = self.run_carry(new)
        self.assertEqual(len(moved), 2, moved)
        self.assertEqual(sorted(p.name for p in (new / 'pipelines').iterdir()), ['b.keys', 'b.metalarchive'])
        self.assertEqual((new / 'pso_collected.tsv').read_text(), 'R\tnewer\n')
        self.assertFalse((self.cache / 'rt-v28-bbb/pipelines').exists())        # moved, not copied
        self.assertTrue((self.cache / 'rt-v27-aaa/pipelines').exists())         # an older one is left alone
        self.assertTrue((self.cache / 'rt-v28-bbb/old-translation_b.metal').exists())   # translations stay per folder
        self.assertFalse((new / 'old-translation_b.metal').exists())
        self.assertEqual(self.run_carry(new), [])                                # once only

    def test_a_folder_with_its_own_archive_keeps_it(self):
        self.folder('rt-v28-bbb', ['b.metalarchive'], 'R\tnewer\n')
        new = self.folder('rt-v29-ccc', ['own.metalarchive'])
        moved = self.run_carry(new)
        self.assertEqual([Path(m).name for m in moved], ['pso_collected.tsv'])
        self.assertEqual([p.name for p in (new / 'pipelines').iterdir()], ['own.metalarchive'])

    def test_a_packaging_cache_takes_nothing(self):
        self.folder('rt-v28-bbb', ['b.metalarchive'], 'R\tx\n')
        other = self.cache / 'archive'; other.mkdir()
        self.assertEqual(self.run_carry(other), [])
        self.assertTrue((self.cache / 'rt-v28-bbb/pipelines/b.metalarchive').exists())

    def test_the_runtime_calls_it_for_its_folder(self):
        rt = (NVN / 'totk_shader_rt.m').read_text()
        self.assertIn('#include "totk_rt_carry.h"', rt)
        self.assertIn('totk_rt_carry_over(g_dir)', rt)


if __name__ == '__main__':
    unittest.main()
