"""Odyssey audio smoothing, HUD modes and packaged render-pipeline warm-up regressions."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
NVN = GAME / "third_party/d2r_gfx/gfx/nvn"


@unittest.skipUnless(sys.platform == "darwin", "production Mac runtime")
class RuntimeProbe(unittest.TestCase):
    def compile_probe(self, target, sources, flags=()):
        result = subprocess.run(["xcrun", "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                 "-fsanitize=undefined", "-fno-sanitize-recover=all", "-Wl,-dead_strip",
                                 "-I" + str(GAME / "runtime"), "-I" + str(NVN), "-I" + str(NVN / "include"),
                                 *map(str, sources), *flags, "-o", str(target)], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_hud_modes_resolution_and_pipeline_counts(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / "hud"
            self.compile_probe(binary, [GAME / "tests/hud_updates/probe.c", NVN / "fps_overlay.c"], ["-framework", "CoreFoundation"])
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
