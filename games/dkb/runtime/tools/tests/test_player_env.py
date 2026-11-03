"""Saved diagnostics are filtered only for the opt-in player IPA."""
import pathlib
import subprocess
import tempfile
import unittest


class PlayerEnvironment(unittest.TestCase):
    def test_overlays_and_player_options_survive(self):
        header = pathlib.Path(__file__).resolve().parents[2] / 'ios/Sources/player_env.h'
        source = r'''
#include <assert.h>
#include "player_env.h"
int main(void) {
 const char *drop[]={"DM_NVN_CPUPROF","DM_NVN_TSV","DM_NVN_PASSDUMP_ALL",
   "DM_NVN_GEOMETRY_PACK","DM_NVN_SHMAP","DM_NVN_ZBLOCK","DM_NVN_ENC_THREAD_PROBE","DM_CTL",
   "DM_TEST_UP_LOST","MTL_DEBUG_LAYER","MTL_SHADER_VALIDATION","DM_MSL_DIR"};
 const char *keep[]={"DM_SHOW_FPS","DM_METAL_HUD","MTL_HUD_ENABLED",
   "DM_FPS_TARGET","DM_DYNAMIC_RESOLUTION","DM_OPMODE","DM_RENDER_SCALE","DM_LANG","DM_RT",
   "DM_TOUCH_HAPTICS","DM_RUMBLE","DM_SAVE","DM_ROMFS","DM_NVN_ENC_THREAD"};
 for(unsigned i=0;i<sizeof drop/sizeof *drop;i++)assert(dm_player_diagnostic_key(drop[i]));
 for(unsigned i=0;i<sizeof keep/sizeof *keep;i++)assert(!dm_player_diagnostic_key(keep[i]));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = pathlib.Path(tmp) / 'test.c'
            exe = pathlib.Path(tmp) / 'test'
            c.write_text(source)
            subprocess.run(['cc', '-Wall', '-Werror', '-I', str(header.parent), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
