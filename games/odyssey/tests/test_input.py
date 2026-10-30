"""Odyssey's copy of the Mac input code (runtime: gamepad.m, gamepad_layout.h, mouse_look.h) runs the probes games/totk
has for it: the face-button layout, the controller type on key 6 and mouse mode on key 7.

Run with Xcode selected: python -m unittest discover -s games/odyssey/tests.
No game files, save data or controller are required.
"""
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
    def run_probe(self, command, with_data):
        with tempfile.TemporaryDirectory(prefix="odyssey-input-") as folder:
            probe, data = Path(folder) / "probe", Path(folder) / "data"
            data.mkdir()
            build = subprocess.run(command + ["-o", str(probe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr[-2000:])
            run = subprocess.run([str(probe)] + ([str(data)] if with_data else []), capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(json.loads(run.stdout.strip().splitlines()[-1])["status"], "pass")

    def test_layout(self):
        self.run_probe(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(GAME / "runtime"),
                        str(GAME / "tests/gamepad_layout/probe.c")], False)

    def test_controller_type(self):
        self.run_probe(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(GAME / "tests/gamepad_type/probe.m"), *OBJC], True)

    def test_mouse_look(self):
        self.run_probe(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(GAME / "runtime"),
                        str(GAME / "tests/mouse_look/probe.c")], False)

    def test_mouse_mode(self):
        self.run_probe(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(GAME / "tests/mouse/probe.m"), *OBJC], True)


if __name__ == "__main__":
    unittest.main()
