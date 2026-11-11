"""#1901: a map of a CPU-visible pool brings the GPU's renderings into the CPU's view.

The Luncheon Kingdom poison map is rendered by the GPU and read by the CPU through nvnMemoryPoolMap with no
invalidate; without the write-back at map time the CPU counted stale bytes as poison. No game files required.
"""
from pathlib import Path
import re
import unittest

NVN = Path(__file__).resolve().parents[1] / "third_party/d2r_gfx/gfx/nvn"


class PoolMapWriteBack(unittest.TestCase):
    def test_map_writes_back_pending_renderings(self):
        hle = (NVN / "hle_nvn.c").read_text()
        map_fn = re.search(r"NVN\(nvnMemoryPoolMap,.*?\n return v->mem;\}", hle, re.S)
        self.assertIsNotNone(map_fn, "nvnMemoryPoolMap changed shape")
        self.assertIn("mtl_pool_map(v)", map_fn.group(0))
        mtl = (NVN / "nvn_mtl.m").read_text()
        body = re.search(r"void mtl_pool_map\(const pool_t \*p\) \{.*?\n\}", mtl, re.S)
        self.assertIsNotNone(body, "mtl_pool_map is missing")
        self.assertIn("mtl_pool_invalidate(p, 0, p->size)", body.group(0))


if __name__ == "__main__":
    unittest.main()
