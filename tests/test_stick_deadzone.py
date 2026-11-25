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
/* Reference: upstream/main stick_axis(), before the configurable deadzone. */
static int32_t main_axis(float value) {
 const float deadzone = 0.12f;
 float magnitude = fabsf(value);
 if (magnitude <= deadzone) return 0;
 if (magnitude > 1.0f) magnitude = 1.0f;
 int32_t result = (int32_t)((magnitude - deadzone) / (1.0f - deadzone) * 32767.0f);
 return value < 0 ? -result : result;
}
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
 assert(!source_capture_active(0,.10f,.10f,0));
 assert(!source_capture_active(1,.20f,0,0));
 assert(!source_capture_active(1,.20f,0,1));
 assert(source_capture_active(1,.35f,0,1));
 assert(source_capture_active(0,.13f,0,0));
 assert(!source_capture_active(0,.00004f,0,1));
 assert(source_capture_active(0,.2f,0,1));
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
 /* Both sticks and both signs, including full diagonals and a mixed near-centre input. */
 for (int i=-128;i<=128;i++) for (int j=-128;j<=128;j++) {
  float x=i/128.f,y=j/128.f;
  assert(stick_axis(x)==main_axis(x));
  assert(native_stick_component(STICK_LX,x,y,0)==main_axis(x));
  assert(native_stick_component(STICK_LY,x,y,0)==main_axis(y));
  assert(native_stick_component(STICK_RX,x,y,0)==main_axis(x));
  assert(native_stick_component(STICK_RY,x,y,0)==main_axis(y));
 }
 assert(native_stick_component(STICK_LX,1,1,0)==32767);
 assert(native_stick_component(STICK_LY,.9f,.1f,0)==0);
 assert(native_stick_component(STICK_LX,1,1,1)<32767);
 runtime_gamepad_stick_deadzone_set(1,.4f);expect(stickDeadzone[0],.12f);expect(stickDeadzone[1],.4f);
 puts("independent stick deadzones: pass");
}
'''


CC = ['xcrun', 'clang'] if shutil.which('xcrun') else ['cc']


@unittest.skipUnless(shutil.which(CC[0]), 'needs a C compiler')
class StickDeadzoneTests(unittest.TestCase):
    def test_runtime_and_persistence(self):
        source = (RUNTIME / 'gamepad_core.h').read_text()
        settings = (RUNTIME / 'gamepad_settings.h').read_text()
        def section(start, end):
            return source[source.index(start):source.index(end, source.index(start))]
        code = PRELUDE + section('enum { STICK_LX', 'static int32_t stronger_axis(')
        code = code.replace('#endif\nstatic inline int32_t stick_axis_deadzone', '\nstatic inline int32_t stick_axis_deadzone', 1)
        code += settings[settings.index('static int setting_path('):settings.index('/* A setting both games share')]
        code += settings[settings.index('static int setting_read('):settings.index('/* The same one-word files')]
        code += section('static const char *stick_deadzone_file(', 'void runtime_gamepad_stick_preview(')
        code += section('static int read_stick_deadzone(', 'void runtime_gamepad_stick_raw(')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'probe.c').write_text(code + MAIN)
            subprocess.run(CC + ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', f'-I{RUNTIME}', str(path / 'probe.c'),
                            '-o', str(path / 'probe'), '-lm'], check=True, timeout=120)
            result = subprocess.run([str(path / 'probe'), str(path / 'controls.txt')],
                                    check=True, capture_output=True, text=True, timeout=30)
            self.assertIn('independent stick deadzones: pass', result.stdout)

if __name__ == '__main__':
    unittest.main()
