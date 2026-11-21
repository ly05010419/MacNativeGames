#!/usr/bin/env python3
# scripts/clean_metal_cache.py: which of Metal's build folders go, on a fake cache folder (no real Metal state is touched)
import importlib.util, os, tempfile, unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('clean_metal_cache', REPO / 'scripts/clean_metal_cache.py')
assert spec is not None and spec.loader is not None
cmc = importlib.util.module_from_spec(spec); spec.loader.exec_module(cmc)

NOW = 2_000_000_000.0


def folder(root, name, changed, inner=None):   # a gpuarchiver-* folder, last changed at `changed`; inner: a file deeper down changed at that time instead
    d = root / name; (d / 'reflection').mkdir(parents=True)
    f = d / 'reflection' / 'r0'; f.write_bytes(b'x' * 4096)
    for p in (f, d / 'reflection', d): os.utime(p, (changed, changed))
    if inner is not None: os.utime(f, (inner, inner))
    return d


class CleanTest(unittest.TestCase):
    def test_idle_and_older_than_every_host(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            old = folder(root, 'gpuarchiver-aaaaaa', NOW - 5 * 3600)
            recent = folder(root, 'gpuarchiver-bbbbbb', NOW - 10 * 60)                       # changed within the idle time
            deep = folder(root, 'gpuarchiver-cccccc', NOW - 5 * 3600, inner=NOW - 10 * 60)   # only a file inside changed lately
            host = folder(root, 'gpuarchiver-dddddd', NOW - 2 * 3600)                        # idle, but after a running host started
            other = root / 'unrelated'; other.mkdir(); os.utime(other, (0, 0))
            link = root / 'gpuarchiver-eeeeee'; link.symlink_to(other)
            n, size, kept, bad = cmc.clean(root, idle_min=60, now=NOW, starts=[NOW - 3 * 3600], say=lambda m: None)
            self.assertEqual((n, kept, bad), (1, 3, 0))
            self.assertGreaterEqual(size, 4096)
            self.assertFalse(old.exists())
            for p in (recent, deep, host, other, link): self.assertTrue(p.exists(), p)

    def test_dry_run_and_no_hosts(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); d = folder(root, 'gpuarchiver-aaaaaa', NOW - 2 * 3600)
            self.assertEqual(cmc.clean(root, idle_min=60, dry_run=True, now=NOW, starts=[])[0], 1)
            self.assertTrue(d.exists())
            self.assertEqual(cmc.clean(root, idle_min=60, now=NOW, starts=[])[0], 1)
            self.assertFalse(d.exists())

    def test_missing_folder(self):
        self.assertEqual(cmc.clean('/nonexistent/com.apple.gpuarchiver', now=NOW, starts=[]), (0, 0, 0, 0))

    def test_hosts_from_ps(self):
        self.assertEqual(cmc.etime_s('05:07'), 307)
        self.assertEqual(cmc.etime_s('02:05:07'), 7507)
        self.assertEqual(cmc.etime_s('3-02:05:07'), 266707)
        out = '  04:51 /Volumes/T7/nsp/The Legend of Zelda [x]/regress/host/efe4/dmhost\n  1-00:00:00 /sbin/launchd\n'
        with mock.patch.object(cmc.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout=out, stderr='')):
            self.assertEqual(cmc.host_starts(NOW), [NOW - 291])
        with mock.patch.object(cmc.subprocess, 'run', return_value=mock.Mock(returncode=1, stdout='', stderr='no')):
            with tempfile.TemporaryDirectory() as td:
                folder(Path(td), 'gpuarchiver-aaaaaa', NOW - 5 * 3600)
                self.assertRaises(OSError, cmc.clean, td, now=NOW)   # nothing is removed without the process list
                self.assertTrue((Path(td) / 'gpuarchiver-aaaaaa').exists())


if __name__ == '__main__':
    unittest.main()
