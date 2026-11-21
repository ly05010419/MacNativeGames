"""NSP script: the free-space check's estimate of the extraction (extract_bytes). Two NSPs of the same size both count (the sizes were once collected in a set, PR #277);
with clones the base game counts once, without them once per version. No game files, keys or Xcode needed (temporary files only)."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec is not None and spec.loader is not None
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


class Space(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)

    def nsp(self, name, size):
        p = self.tmp / name; p.write_bytes(bytes(size)); return p

    def test_same_size_nsps_both_count(self):
        base, upd = self.nsp('base.nsp', 4), self.nsp('update.nsp', 4)
        for clones in (True, False): self.assertEqual(nsp.extract_bytes({'v0': (base, None), 'v1': (base, upd)}, clones), 12 - 4 * clones)
        self.assertEqual(nsp.extract_bytes({'v1': (base, upd)}, True), 8)

    def test_base_once_with_clones(self):
        base, u1, u2 = self.nsp('base.nsp', 100), self.nsp('u1.nsp', 5), self.nsp('u2.nsp', 6)
        self.assertEqual(nsp.extract_bytes({'v0': (base, None), 'v1': (base, u1), 'v2': (base, u2)}, True), 111)

    def test_base_per_version_without_clones(self):
        base, u1, u2 = self.nsp('base.nsp', 100), self.nsp('u1.nsp', 5), self.nsp('u2.nsp', 6)
        self.assertEqual(nsp.extract_bytes({'v0': (base, None), 'v1': (base, u1), 'v2': (base, u2)}, False), 311)

    def test_no_clone_source_without_v0(self):   # clones need the base game extracted alone first
        base, u1, u2 = self.nsp('base.nsp', 100), self.nsp('u1.nsp', 5), self.nsp('u2.nsp', 6)
        self.assertEqual(nsp.extract_bytes({'v1': (base, u1), 'v2': (base, u2)}, True), 211)


if __name__ == '__main__':
    unittest.main()
