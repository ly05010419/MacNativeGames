"""CPU checks for the pipeline allowance and opt-in scheduler counters."""
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT / "games/totk/third_party/d2r_gfx/gfx/nvn/include"


class PipelineBudget(unittest.TestCase):
    def test_allowance_exhaustion_reset_and_control(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        with tempfile.TemporaryDirectory() as folder:
            binary = str(Path(folder) / "budget")
            build = subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=undefined", "-fno-sanitize-recover=all",
                                    str(ROOT / "tests/test_pipeline_scan_budget.c"), "-o", binary],
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


@unittest.skipUnless(sys.platform == "darwin" and platform.machine() == "arm64", "native Apple scheduler")
class CoreWaitStats(unittest.TestCase):
    def test_both_games_enabled_and_disabled(self):
        for defines in ([], ["-DGAME_BOTW=1"]):
            with self.subTest(defines=defines), tempfile.TemporaryDirectory() as folder:
                binary = str(Path(folder) / "core-stats")
                build = subprocess.run(["xcrun", "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                        "-I" + str(INCLUDE), *defines, "-Wl,-dead_strip",
                                        str(ROOT / "tests/test_core_wait_stats.c"), "-o", binary],
                                       capture_output=True, text=True, timeout=60)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                for on in ("0", "1", "2"):
                    result = subprocess.run([binary, on], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
