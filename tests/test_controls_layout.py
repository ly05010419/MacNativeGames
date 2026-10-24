"""Core shared controls.txt, runtime and editor probes. No retail game data."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

class ControlsLayoutTests(unittest.TestCase):
    def test_file_roundtrip(self):
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/'layout'
            subprocess.run(['xcrun','clang','-std=c11','-Wall','-Wextra','-Werror',str(ROOT/'tests/test_controls_layout.c'),'-o',str(exe)],check=True,capture_output=True)
            subprocess.run([str(exe)],check=True,capture_output=True)

    def test_runtime_input(self):
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/'input'
            sources=['scripts/input_probe.c','runtime/input.c','runtime/hid_feedback.c','runtime/motion_fusion.c','runtime/autoload.c']
            build=subprocess.run(['xcrun','clang','-std=c11','-O1','-Wall','-Wextra','-Werror',*[str(ROOT/'games/totk'/s) for s in sources],'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True,env={k:v for k,v in os.environ.items() if not k.startswith('TOTK_')},timeout=30)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)

    @unittest.skipUnless(os.environ.get('TOTK_RUN_APPKIT_PROBES')=='1','opt-in native AppKit sheet probe')
    def test_editor(self):
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/'editor'
            command=['xcrun','clang','-std=c11','-fobjc-arc','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-I'+str(ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn/include'),str(ROOT/'tests/controls_editor_probe.m')]
            for framework in ['Cocoa','QuartzCore','Metal','CoreVideo']:command+=['-framework',framework]
            build=subprocess.run(command+['-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            run=subprocess.run([str(exe),folder],capture_output=True,text=True,timeout=30)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
