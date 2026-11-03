"""Odyssey's production controller layout, saved type and mouse handling. No game files or controller required."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
OBJC = ["-fobjc-arc", "-framework", "Foundation", "-framework", "GameController", "-framework", "IOKit"]


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("xcrun"), "needs macOS and the Xcode command line tools")
class OdysseyInput(unittest.TestCase):
    def run_probe(self, command, with_data, *args):
        with tempfile.TemporaryDirectory(prefix="odyssey-input-") as folder:
            probe, data = Path(folder) / "probe", Path(folder) / "data"
            data.mkdir()
            build = subprocess.run(command + ["-o", str(probe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr[-2000:])
            run = subprocess.run([str(probe)] + ([str(data)] if with_data else []) + list(args), capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            if run.stdout.strip().startswith("{"):
                self.assertEqual(json.loads(run.stdout.strip().splitlines()[-1])["status"], "pass")
            else:
                self.assertIn("pass:", run.stdout)

    def test_keyboard_mapping(self):
        self.run_probe(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(GAME / "tests/keyboard_map/probe.c")], False)

    def test_layout(self):
        self.run_probe(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(GAME / "runtime"),
                        str(GAME / "tests/gamepad_layout/probe.c")], False)

    def test_controller_type(self):
        self.run_probe(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(GAME / "tests/gamepad_type/probe.m"), *OBJC], True)

    def run_npad_probe(self, *args):
        self.run_probe(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-Wl,-dead_strip",
                        "-Wl,-undefined,dynamic_lookup", str(GAME / "tests/two_player/probe.m"), *OBJC], False, *args)

    def test_two_player(self):
        self.run_npad_probe()

    def test_guest_disconnect_clears_only_the_target(self):
        self.run_npad_probe("--disconnect")

    def run_feedback_probe(self, *args):
        self.run_probe(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-Wl,-dead_strip",
                        "-Wl,-undefined,dynamic_lookup", str(GAME / "tests/feedback_routing/probe.m"),
                        str(GAME / "runtime/motion_fusion.c"), *OBJC, "-framework", "CoreHaptics"], False, *args)

    def test_later_controller_does_not_take_rumble(self):
        self.run_feedback_probe()

    def test_first_capable_controller_keeps_motion(self):
        self.run_feedback_probe("--motion")

    def test_mouse_look(self):
        self.run_probe(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(GAME / "runtime"),
                        str(GAME / "tests/mouse_look/probe.c")], False)

    def test_mouse_mode(self):
        self.run_probe(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(GAME / "tests/mouse/probe.m"), *OBJC], True)


if __name__ == "__main__":
    unittest.main()
