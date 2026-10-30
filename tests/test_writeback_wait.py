"""Write-back wait handoff with the production wait helper, fence macro and scheduler."""
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / "games/totk/third_party/d2r_gfx/gfx/nvn"


@unittest.skipUnless(sys.platform == "darwin" and platform.machine() == "arm64", "native Apple scheduler")
class WritebackWait(unittest.TestCase):
    def test_guest_nested_and_host_waits(self):
        source = (NVN / "nvn_mtl.m").read_text()
        helper = re.search(r"static void wb_wait_gpu\(.*?\n}\n", source, re.S).group()
        fixture = (ROOT / "tests/writeback_wait_probe.c").read_text()
        for defines in ([], ["-DGAME_BOTW=1"], ["-DGAME_BOTW=1", "-DGAME_BOTW190=1"]):
            with self.subTest(defines=defines), tempfile.TemporaryDirectory() as folder:
                probe = Path(folder) / "probe.c"
                probe.write_text(fixture.replace("/* PRODUCTION_WAIT */", helper))
                binary = str(Path(folder) / "probe")
                result = subprocess.run([
                    "xcrun", "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wl,-dead_strip", *defines,
                    "-I" + str(ROOT / "games/totk/runtime"), "-I" + str(NVN), "-I" + str(NVN / "include"),
                    str(probe), "-o", binary,
                ], capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for full in ("0", "1"):
                    run = subprocess.run([binary, full], capture_output=True, text=True, timeout=15)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
