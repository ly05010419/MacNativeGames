"""Execute the real handheld patch against synthetic modules; no game data needed."""
import copy
import plistlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games/totk/runtime'
TOOLS = ROOT / 'games/totk/ios/tools'
sys.path.insert(0, str(TOOLS))
import build_ipa


def function(source, name):
    start = source.index(name + ' {')
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class HandheldScene(unittest.TestCase):
    def test_export_and_device_use_the_same_read_only_code(self):
        source = (RUNTIME / 'game_patches_botw.c').read_text()
        functions = '\n'.join(function(source, name) for name in (
            'static uint32_t adrp_reg(unsigned rd, uint64_t site, uint64_t target)',
            'static uint64_t output_floats(unsigned screen, unsigned render)',
            'static void put_words(Module *m, uint64_t off, const uint32_t *words, size_t n, const char *what)',
            'static void put_word(Module *m, uint64_t off, uint32_t word, const char *what)',
            'static void patch_phone_render(Module *m)',
        ))
        prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "botw_handheld_scene.h"
#define TOTK_GAME_VERSION "synthetic"
typedef struct { uint8_t *base; uint64_t file_size, size; } Module;
static int ios_code(void) { return 1; }
static void runtime_fail(const char *s) { fprintf(stderr,"%s\n",s); exit(42); }
'''
        harness = r'''
int main(int argc, char **argv) {
    Module m = {.file_size=0x2000000, .size=0x2004000};
    assert(!posix_memalign((void **)&m.base,0x4000,m.size));
    memset(m.base,0xa5,m.size);
    const char *values[]={NULL,"960x540","800x450","640x360","0","1280x720","800x450x","", "-1"};
    unsigned widths[]={960,960,800,640,960,960,960,960,960};
    for(unsigned i=0;i<sizeof widths/sizeof *widths;i++) {
        if(values[i]) setenv("TOTK_BOTW_HANDHELD_SCENE",values[i],1);
        else unsetenv("TOTK_BOTW_HANDHELD_SCENE");
        uint32_t *code=(void *)(m.base+PHONE_RENDER);
        code[0]=CODE_PATCHED_AT_BUILD ? adrp_reg(8,(uintptr_t)code,(uintptr_t)m.base+m.file_size) : 0xd2a89408u;
        code[1]=CODE_PATCHED_AT_BUILD ? BOTW_HANDHELD_LOAD : 0xf2e89408u;
        code[2]=PHONE_RENDER_STR;
        uint32_t before[3]; memcpy(before,code,sizeof before);
        if(argc>1 && !strcmp(argv[1],"bad-word")) code[0]^=1;
        if(argc>1 && !strcmp(argv[1],"bad-layout")) m.size-=0x4000;
        patch_phone_render(&m);
        if(CODE_PATCHED_AT_BUILD) assert(!memcmp(before,code,sizeof before));
        assert(code[2]==PHONE_RENDER_STR);
        // Decode ADRP + LDR and follow the address as the guest CPU does.
        uint32_t w=code[0], load=code[1];
        assert((w&0x9f00001fu)==0x90000008u);
        int64_t imm=((w>>29)&3u)|(((w>>5)&0x7ffffu)<<2);
        if(imm&(1<<20)) imm-=(1<<21);
        uintptr_t page=((uintptr_t)code&~(uintptr_t)0xfff)+(imm*4096);
        assert((load&0xffc003ffu)==0xf9400108u);
        uintptr_t address=page+(((load>>10)&0xfffu)*8);
        assert(address==(uintptr_t)m.base+m.file_size+0x28);
        float pair[2];memcpy(pair,(void *)address,8);
        assert(pair[0]==1280.0f && pair[1]==widths[i]);
        for(unsigned j=0;j<0x50;j++) if(j<0x28 || j>=0x30)
            assert(m.base[m.file_size+j]==0xa5); // adjacent state stays intact
    }
    free(m.base);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            c = Path(d)/'probe.c'
            for version, site, store in ((160, 0x18216c8, 0xf9094b88), (190, 0x1266114, 0xf9090a68)):
                for device in (0, 1):
                    with self.subTest(version=version, device=device):
                        c.write_text(prelude + f'\n#define CODE_PATCHED_AT_BUILD {device}\nenum {{PHONE_RENDER={site},PHONE_RENDER_STR={store}u}};\n' + functions + harness)
                        executable = Path(d)/'probe'
                        subprocess.run(['xcrun','clang','-std=c11','-Wall','-Wextra','-Werror',
                                        '-fsanitize=undefined','-fno-sanitize-recover=all','-I',str(RUNTIME),str(c),'-o',str(executable)],check=True,capture_output=True,text=True)
                        subprocess.run([str(executable)],check=True,capture_output=True,text=True)
                        for failure in ('bad-word','bad-layout'):
                            result=subprocess.run([str(executable),failure],capture_output=True,text=True)
                            self.assertEqual(result.returncode,42,result.stderr)

    def test_only_botw_offers_the_setting_and_keeps_its_default(self):
        original=plistlib.loads((TOOLS.parent/'Settings.bundle/Root.plist').read_bytes())
        for root in (original,build_ipa.bananza_pane(copy.deepcopy(original))):
            self.assertNotIn('TOTK_BOTW_HANDHELD_SCENE',{s.get('Key') for s in root['PreferenceSpecifiers']})
        root=build_ipa.botw_pane(copy.deepcopy(original))
        rows=[s for s in root['PreferenceSpecifiers'] if s.get('Key')=='TOTK_BOTW_HANDHELD_SCENE']
        self.assertEqual(len(rows),1)
        self.assertEqual(rows[0]['Values'],['960x540','800x450','640x360'])
        self.assertEqual(rows[0]['DefaultValue'],'960x540')
        self.assertTrue(rows[0]['TOTKPhone'])


if __name__=='__main__':
    unittest.main()
