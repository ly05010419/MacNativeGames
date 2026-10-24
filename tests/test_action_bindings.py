"""Core action-layer regression: execute generated veneers on the native ARM CPU."""
from pathlib import Path
import os
import platform
import subprocess
import tempfile
import unittest

@unittest.skipUnless(platform.system() == "Darwin" and platform.machine() == "arm64", "Native ARM64 macOS veneers")
class PlayerActions(unittest.TestCase):
    def test_live_actions_and_version_guard(self):
        root = Path(__file__).resolve().parents[1]
        env = {k: v for k, v in os.environ.items() if k not in ("TOTK_INPUT_FILE", "TOTK_INPUT_SCRIPT", "TOTK_ACTION_LAYOUT", "TOTK_EXPORT_IOS", "TOTK_ACTION_MAP", "TOTK_BUTTON_MAP", "TOTK_PLAYER_ACTIONS", "TOTK_CONTROLS")}
        with tempfile.TemporaryDirectory() as folder:
            for defines in ([], ["-DTOTK_GAME_V112=1"]):
                exe = Path(folder) / ("actions-old" if defines else "actions")
                command = ["xcrun", "clang", "-std=c11", "-ffunction-sections", "-fdata-sections", "-Wl,-dead_strip", *defines,
                           str(root / "tests/test_action_bindings.c"), str(root / "tests/action_stub_probe.S"), str(root / "games/totk/runtime/semantic_action_stub.S"), "-o", str(exe)]
                build = subprocess.run(command, capture_output=True, text=True, timeout=60)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                run = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=15)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("PASS:", run.stdout)

    def test_v143_gate_and_hooks(self):
        root = Path(__file__).resolve().parents[1]
        env = {k: v for k, v in os.environ.items() if k not in ("TOTK_INPUT_FILE", "TOTK_INPUT_SCRIPT", "TOTK_ACTION_LAYOUT", "TOTK_EXPORT_IOS", "TOTK_ACTION_MAP", "TOTK_BUTTON_MAP", "TOTK_PLAYER_ACTIONS", "TOTK_CONTROLS")}
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / "actions-v143"
            command = ["xcrun", "clang", "-std=c11", "-ffunction-sections", "-fdata-sections", "-Wl,-dead_strip", "-DTOTK_GAME_V143=1",
                       str(root / "tests/test_action_bindings_v143.c"), str(root / "tests/action_stub_probe_v143.S"),
                       str(root / "games/totk/runtime/semantic_action_stub.S"), "-o", str(exe)]
            build = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("PASS: 1.4.3", run.stdout)


    def test_opt_in_and_saved_actions(self):
        root=Path(__file__).resolve().parents[1]
        env={k:v for k,v in os.environ.items() if not k.startswith("TOTK_")}
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/"settings"
            build=subprocess.run(["xcrun","clang","-std=c11","-Wall","-Wextra","-Werror",str(root/"tests/action_settings_probe.c"),"-o",str(exe)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            controls=Path(folder)/"controls.txt"
            def check(enabled,action,extra=None):
                run=subprocess.run([str(exe),str(controls),str(enabled),str(action)],capture_output=True,text=True,env=env|dict(extra or {}),timeout=10)
                self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            check(0,1)
            check(1,1,{"TOTK_PLAYER_ACTIONS":"1"})
            file=Path(folder)/"player-actions.txt"
            file.write_text("4,2,4,8,16,32,64,128,256,512,1024,2048,4096,8192,16384,32768\n")
            check(1,4)
            check(0,4,{"TOTK_PLAYER_ACTIONS":"0"})
            file.write_text("invalid")
            check(0,1)

    @unittest.skipUnless(os.environ.get("TOTK_RUN_APPKIT_PROBES")=="1","opt-in AppKit action sheet probe")
    def test_action_editor(self):
        root=Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/"editor"
            command=["xcrun","clang","-std=c11","-fobjc-arc","-Wall","-Wextra","-Werror","-Wno-unused-function","-Wno-unused-variable","-Wno-deprecated-declarations","-I"+str(root/"games/totk/third_party/d2r_gfx/gfx/nvn/include"),str(root/"tests/action_editor_probe.m")]
            for framework in ["Cocoa","QuartzCore","Metal","CoreVideo"]:command += ["-framework",framework]
            build=subprocess.run(command+["-o",str(exe)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            run=subprocess.run([str(exe),folder],capture_output=True,text=True,env={k:v for k,v in os.environ.items() if not k.startswith("TOTK_")},timeout=30)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
