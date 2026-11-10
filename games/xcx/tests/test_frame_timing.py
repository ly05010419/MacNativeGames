"""Exercise the loaded-code patch and real-time camera clock without game data."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "runtime.h"
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
static jmp_buf failed;
static int movie;
_Noreturn void runtime_fail(const char *s) { (void)s; longjmp(failed, 1); }
int movie_host_playing(void) { return movie; }
void runtime_game_patches(Module *);
static const uint32_t sites[][2] = {
 {0x13db540,0x52800040},{0x13baa34,0x72a79108},{0x13a9af4,0x1e210800},
 {0xa1f05c,0x1e27d001},{0x56d40,0x72a7a108},{0x1e28e0,0x1e204100},
 {0x1e291c,0x1e204100},{0xeb408,0x1e204100},{0x5c6284,0x1e213800},
 {0xab700,0xbd40b6a8},{0xe5e8c,0x1e280800},{0xe5860,0x1e214000},
 {0xe5eec,0x1e3e1001},{0xe5ef0,0x1e212800},{0x23efe8,0x5286f7a8},
 {0x23efec,0x72b6b0c8},{0x23eff8,0x1e270101},{0x23effc,0x1e212800},
 {0x927e4c,0x1e201821},{0x927e50,0x1e201840},{0xa1116c,0xbd489501},
 {0xa11180,0xbd489501},{0xa1126c,0xbd489500}
};
static float value(Module *m, unsigned index) {
 float f; memcpy(&f,m->base+0x4800000+0x40+index*4,4); return f;
}
static void near(double a,double b) { assert(fabs(a-b)<0.0001); }
int main(int argc,char **argv) {
 assert(argc==2); const char *mode=argv[1];
 Module m={.name="main",.file_size=0x4800000,.size=0x4804000,
  .text_end=0x1735050,.ro_off=0x1736000};
 m.base=mmap(NULL,m.size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 assert(m.base!=MAP_FAILED && !((uintptr_t)m.base&0x3fff));
 for(unsigned i=0;i<sizeof sites/sizeof *sites;i++)memcpy(m.base+sites[i][0],&sites[i][1],4);
 if(!strcmp(mode,"word"))m.base[sites[22][0]]^=1;
 if(!strcmp(mode,"padding"))m.base[m.text_end+16]=1;
 if(!strcmp(mode,"short"))m.size=m.file_size;
 unsigned char *before=malloc(m.size);assert(before);memcpy(before,m.base,m.size);
 int reject=!strcmp(mode,"word")||!strcmp(mode,"padding")||!strcmp(mode,"short")||!strcmp(mode,"reject");
 if(setjmp(failed)) { assert(reject);assert(!memcmp(before,m.base,m.size));return 0; }
 runtime_game_patches(&m);assert(!reject);
 if(!strcmp(mode,"off")) { assert(!memcmp(before,m.base,m.size));return 0; }
 unsigned changed=0;
 for(unsigned i=0;i<sizeof sites/sizeof *sites;i++) {
  uint32_t w;memcpy(&w,m.base+sites[i][0],4);changed+=w!=sites[i][1];
 }
 assert(changed==23);
 double hz=atof(getenv("XCX_FPS_LIMIT"));near(value(&m,0),1/hz);near(value(&m,3),30/hz);
 uint64_t now=1000000000;runtime_frame_step_update(now);
 double camera=0,elapsed=0;
 for(unsigned i=0;i<600;i++) {
  /* Include a slower section: simulation must follow elapsed time, not the cap. */
  uint64_t delta=i<300?(uint64_t)(1e9/hz):33333333;
  now+=delta;elapsed+=delta/1e9;runtime_frame_step_update(now);
  camera+=value(&m,3);
 }
 /* A five-frame average delays a step change by two frame periods. */
 assert(fabs(camera-30*elapsed)<2*30*(1.0/30-1/hz)+0.001);
 uint32_t cut=2;memcpy(m.base+0x1fefaf4,&cut,4);runtime_frame_step_update(now+=33333333);
 near(value(&m,0),1.0/60);near(value(&m,2),1.0/30);near(value(&m,3),1);
 uint32_t interval;memcpy(&interval,m.base+0x1d943a0,4);assert(interval==2);
 cut=0;memcpy(m.base+0x1fefaf4,&cut,4);movie=1;runtime_frame_step_update(now+=33333333);
 near(value(&m,3),1);movie=0;runtime_frame_step_update(now+=1000000000);
 near(value(&m,0),1/hz);near(value(&m,3),30/hz); /* reset after loading */
 /* Execute the installed camera trampoline itself, not just its timing data.
    Set the following instruction to RET; all other fixture text is zero. */
#if defined(__aarch64__)
 uint32_t ret=0xd65f03c0;memcpy(m.base+0xab704,&ret,4);
 __builtin___clear_cache((char *)m.base,(char *)m.base+m.ro_off);
 assert(!mprotect(m.base,m.ro_off,PROT_READ|PROT_EXEC));
 extern float run_camera(void *,void *);
 float parameters[46]={0};parameters[45]=3.0f;
 near(run_camera(parameters,m.base+0xab700),3.0*30/hz);
#endif
 free(before);munmap(m.base,m.size);return 0;
}
#if defined(__aarch64__)
__asm__(".text\n.p2align 2\n_run_camera:\n"
 "stp x21,x30,[sp,#-32]!\n stp d8,d16,[sp,#16]\n mov x21,x0\n blr x1\n"
 "fmov s0,s8\n ldp d8,d16,[sp,#16]\n ldp x21,x30,[sp],#32\n ret\n");
#endif
'''


class FrameTimingTest(unittest.TestCase):
    def test_checked_patch_and_camera_clock(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, binary = root / 'probe.c', root / 'probe'
            source.write_text(HARNESS)
            clean = {k: v for k, v in os.environ.items()
                     if k not in ('XCX_FPS_LIMIT', 'DM_PRESENT_HZ')}
            for version in ('2.0.0', '1.0.0'):
                (root / 'xcx_version_config.h').write_text(
                    f'#define TOTK_GAME_VERSION "{version}"\n')
                subprocess.run(['xcrun', 'clang', '-std=c11', '-D_DARWIN_C_SOURCE',
                                '-Wall', '-Wextra', '-Werror', '-I', str(root),
                                '-I', str(ROOT / 'runtime'), str(source),
                                str(ROOT / 'runtime/game_patches.c'), '-o', str(binary)], check=True)
                cases = [('off', {})]
                if version == '2.0.0':
                    cases += [(mode, {'XCX_FPS_LIMIT': '60'})
                              for mode in ('word', 'padding', 'short')]
                    cases += [('reject', {'XCX_FPS_LIMIT': value})
                              for value in ('0', '60x', '144', '-1')]
                    cases += [('clock', {'XCX_FPS_LIMIT': value}) for value in ('60', '120')]
                    cases += [('reject', {'XCX_FPS_LIMIT': '60', 'DM_PRESENT_HZ': '120'})]
                else:
                    cases += [('reject', {'XCX_FPS_LIMIT': '60'})]
                for mode, env in cases:
                    with self.subTest(version=version, mode=mode, env=env):
                        subprocess.run([str(binary), mode], env={**clean, **env}, check=True)
