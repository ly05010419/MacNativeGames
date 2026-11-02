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

    def test_audio_smoothing_and_stream_boundaries(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / "audio"
            self.compile_probe(binary, [GAME / "tests/audio_smoothing/probe.c", GAME / "runtime/audio_effects.c",
                                        GAME / "runtime/audio_reverb.c"], ["-framework", "AudioToolbox"])
            env = {**os.environ, "TOTK_AUDIO_RAMP": "1"}
            good = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(good.returncode, 0, good.stdout + good.stderr)
            self.assertIn('"status":"pass"', good.stdout + good.stderr)
            bad = subprocess.run([str(binary)], env={**env, "TOTK_AUDIO_RAMP": "0"}, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("volume step or stop is not smoothed", bad.stderr)



if __name__ == "__main__":
    unittest.main()
