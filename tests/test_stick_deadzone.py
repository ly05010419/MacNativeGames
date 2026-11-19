"""Compile the runtime's real deadzone/persistence functions with isolated settings."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games/totk/runtime'
PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "runtime.h"
static RuntimeGamepadCalibration calibration = {.left={.scaleX=1, .scaleY=1},.right={.scaleX=1, .scaleY=1}};
static int hidUpdates, refreshes;
static void hid_calibration_changed(void){hidUpdates++;}
static void refresh_gamepads(void){refreshes++;}
'''
MAIN = r'''
static void expect(float a,float b){assert(fabsf(a-b)<1e-6f);}
int main(int argc,char**argv){
 assert(argc==2);assert(setenv("TOTK_CONTROLS",argv[1],1)==0);
 load_stick_deadzone();expect(stickDeadzone[0],.12f);expect(stickDeadzone[1],.12f);
 setting_write("controller-deadzone.txt","0.18","test");
 load_stick_deadzone();expect(stickDeadzone[0],.18f);expect(stickDeadzone[1],.18f);
 runtime_gamepad_stick_deadzone_set(0,.05f);
 expect(runtime_gamepad_stick_deadzone_get(0),.05f);expect(runtime_gamepad_stick_deadzone_get(1),.18f);
 runtime_gamepad_stick_deadzone_set(1,.3f);
 load_stick_deadzone();expect(stickDeadzone[0],.05f);expect(stickDeadzone[1],.3f);
 assert(!source_capture_active(0,.10f,0,0));
 assert(source_capture_active(0,.13f,0,0));
 assert(!stick_capture_active(.00004f,0,0,0));
 assert(stick_capture_active(.2f,0,0,0));
 float x,y;
 runtime_gamepad_stick_preview(0,.2f,0,&x,&y);assert(x>.15f&&x<.16f&&y==0);
 runtime_gamepad_stick_preview(1,.2f,0,&x,&y);assert(x==0&&y==0);
 assert(stick_component(STICK_LX,.2f,0)>0);assert(stick_component(STICK_RX,.2f,0)==0);
 int before=refreshes;
 runtime_gamepad_stick_deadzone_set(0,NAN);runtime_gamepad_stick_deadzone_set(1,INFINITY);
 assert(refreshes==before);expect(stickDeadzone[0],.05f);expect(stickDeadzone[1],.3f);
 runtime_gamepad_stick_deadzone_set(0,-1);runtime_gamepad_stick_deadzone_set(1,1);
 load_stick_deadzone();expect(stickDeadzone[0],0);expect(stickDeadzone[1],.5f);
 /* Invalid per-side files fall back independently, not contaminate the other side. */
 const char*bad[]={"nan","inf","-0.1","0.51","0.1 junk","garbage"};
 for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++){
  setting_write(stick_deadzone_file(0),bad[i],"test");load_stick_deadzone();
  expect(stickDeadzone[0],.18f);expect(stickDeadzone[1],.5f);
 }
 runtime_gamepad_deadzone_set(.24f);load_stick_deadzone();
 expect(runtime_gamepad_deadzone_get(),.24f);expect(stickDeadzone[1],.24f);
 assert(refreshes==hidUpdates);
 /* No controls path: callers can adjust transiently without reading/writing user settings. */
 unsetenv("TOTK_CONTROLS");load_stick_deadzone();expect(stickDeadzone[0],.12f);expect(stickDeadzone[1],.12f);
 runtime_gamepad_stick_deadzone_set(1,.4f);expect(stickDeadzone[0],.12f);expect(stickDeadzone[1],.4f);
 puts("independent stick deadzones: pass");
}
'''


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode command line tools')
class StickDeadzoneTests(unittest.TestCase):
    def test_runtime_and_persistence(self):
        source = (RUNTIME / 'gamepad_core.h').read_text()
        settings = (RUNTIME / 'gamepad_settings.h').read_text()
        def section(start, end):
            return source[source.index(start):source.index(end, source.index(start))]
        code = PRELUDE + section('enum { STICK_LX', '\n#endif\nstatic inline int32_t stick_axis')
        code += settings[settings.index('static int setting_path('):settings.index('/* A setting both games share')]
        code += settings[settings.index('static int setting_read('):settings.index('/* The same one-word files')]
        code += section('static const char *stick_deadzone_file(', 'void runtime_gamepad_stick_raw(')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'probe.c').write_text(code + MAIN)
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', f'-I{RUNTIME}', str(path / 'probe.c'),
                            '-o', str(path / 'probe')], check=True, timeout=120)
            result = subprocess.run([str(path / 'probe'), str(path / 'controls.txt')],
                                    check=True, capture_output=True, text=True, timeout=30)
            self.assertIn('independent stick deadzones: pass', result.stdout)

if __name__ == '__main__':
    unittest.main()
