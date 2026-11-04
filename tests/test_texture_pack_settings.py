"""The production texture-pack startup choice: default off even with a legacy saved 'on'. No game or GPU."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'Mac runtime source probe')
class TexturePackSettings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        folder = Path(cls.temp.name)
        source = folder / 'probe.c'
        source.write_text('''#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "''' + str(ROOT / 'games/totk/runtime/romfs.c') + '''"
static int legacy_reads, path_reads;
int runtime_setting_word(const char *name,char *out,size_t size) {
 if(!strcmp(name,"texture-pack.txt")){legacy_reads++;snprintf(out,size,"on\\n");return 1;}
 if(!strcmp(name,"texture-pack-path.txt")){path_reads++;snprintf(out,size,"/installed-textures\\n");return 1;}
 return 0;
}
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"FAIL: %s\\n",message);exit(2);}
int main(int argc,char **argv) {
 (void)argv;
 unsetenv("TOTK_TEXTURE_PACK");unsetenv("TOTK_TEXTURE_PACK_DIR");
 if(argc==1){
  assert(!runtime_texture_pack_choice());assert(!runtime_texture_pack_active());
  assert(!legacy_reads&&!path_reads);
  setenv("TOTK_TEXTURE_PACK","1",1);assert(runtime_texture_pack_choice());
  assert(!runtime_texture_pack_active()); /* this run already chose stock memory and resources */
 }else{
  setenv("TOTK_TEXTURE_PACK","1",1);assert(runtime_texture_pack_active());
  assert(!legacy_reads&&path_reads==1);
  unsetenv("TOTK_TEXTURE_PACK");assert(!runtime_texture_pack_choice());
  assert(runtime_texture_pack_active()); /* explicit choice remains frozen for the run */
 }
 puts("pass: texture pack startup choice");return 0;
}
''')
        cls.exe = folder / 'probe'
        result = subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                                 '-Wno-deprecated-declarations', '-DTOTK_GAME_V143=1',
                                 '-I' + str(ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/include'),
                                 '-Wl,-dead_strip', '-Wl,-undefined,dynamic_lookup', str(source), '-o', str(cls.exe)],
                                capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check(self, *args):
        env = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
        result = subprocess.run([str(self.exe), *args], env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_legacy_on_is_ignored_and_default_stays_off_for_this_run(self):
        self.check()

    def test_explicit_opt_in_is_frozen_for_this_run(self):
        self.check('explicit')
