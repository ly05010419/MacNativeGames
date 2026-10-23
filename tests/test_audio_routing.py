"""Submix dependency ordering includes active splitter destinations."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "games/totk"


@unittest.skipUnless(sys.platform == "darwin", "production audio mixer uses AudioToolbox")
class AudioRouting(unittest.TestCase):
    def test_splitter_submix_dependencies(self):
        self.check_graphs([])

    def test_botw_stream_boundaries(self):
        self.check_graphs(["-DGAME_BOTW=1"])

    def check_graphs(self, defines):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / "audio-routing-probe"
            build = subprocess.run([
                "xcrun", "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-fno-sanitize-recover=all", *defines, "-I" + str(GAME / "runtime"),
                str(GAME / "tests/audio_routing/probe.c"), str(GAME / "runtime/audio_effects.c"),
                str(GAME / "runtime/audio_reverb.c"), "-framework", "AudioToolbox", "-o", str(binary)
            ], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("4 audio routing graphs", result.stdout)
            self.assertIn("4 EOS sample paths", result.stdout)


if __name__ == "__main__":
    unittest.main()
