#!/usr/bin/env python3
"""Run on macOS: python3 games/totk/tests/command_q/test.py. No ROM or saves needed."""
import pathlib
import subprocess
import tempfile

source = pathlib.Path(__file__).with_name("probe.m")
with tempfile.TemporaryDirectory(prefix="totk-command-q-") as work:
    binary = pathlib.Path(work) / "probe"
    subprocess.run([
        "xcrun", "clang", "-std=c11", "-fobjc-arc", "-Wall", "-Wextra", "-Werror",
        "-Wno-deprecated-declarations", "-I" + str(source.parents[2] / "third_party/d2r_gfx/gfx/nvn/include"), str(source), "-framework", "Cocoa",
        "-framework", "QuartzCore", "-framework", "Metal", "-framework", "CoreVideo",
        "-o", str(binary),
    ], check=True)
    for mode, expected in enumerate(("graceful guest shutdown pending", "save done", "gave up")):
        for text_entry in (0, 1):
            result = subprocess.run([str(binary), str(mode), str(text_entry)],
                                    capture_output=True, text=True, timeout=5)
            assert result.returncode == 0, result.stderr
            assert expected in result.stderr, result.stderr
            assert "[probe] state report: closing" in result.stderr, result.stderr   # reported before _exit
            assert "[probe] triggers released" in result.stderr, result.stderr   # the triggers too: _exit skips atexit
            if mode:
                assert "quitting when the save is done" in result.stderr, result.stderr
            print(f"PASS: save mode {mode}, text entry {text_entry}")
