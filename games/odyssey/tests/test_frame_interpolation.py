"""Focused Odyssey camera, fixed clock and interpolation request regressions."""
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
NVN = GAME / "third_party/d2r_gfx/gfx/nvn"


@unittest.skipUnless(sys.platform == "darwin" and platform.machine() == "arm64", "Apple ARM64 runtime")
class FrameInterpolation(unittest.TestCase):
    def build_run(self, source, flags=()):
        with tempfile.TemporaryDirectory(prefix="odyssey-fi-") as folder:
            exe = Path(folder) / "probe"
            r = subprocess.run(["xcrun", "clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                                "-I" + str(NVN), *flags, str(source), "-o", str(exe)],
                               capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_camera_callback_projection_cut_and_fixed_clock(self):
        source = (NVN / "nvn_mtl.m").read_text()
        a = source.index("static void mfx_take_camera(void) {")
        b = source.index("\nstatic void draw_inner(", a)
        probe = (GAME / "tests/frame_interpolation/camera.c").read_text()
        with tempfile.TemporaryDirectory(prefix="odyssey-camera-") as folder:
            path = Path(folder) / "camera.c"
            path.write_text(probe.replace("PRODUCTION_CAMERA", source[a:b]))
            self.build_run(path, ["-fsanitize=undefined", "-fno-sanitize-recover=all"])

    def test_requested_mode_survives_screen_changes(self):
        self.build_run(GAME / "tests/frame_interpolation/modes.m",
                       ["-fobjc-arc", "-framework", "Foundation", "-framework", "Metal",
                        "-framework", "MetalFX", "-framework", "QuartzCore"])
