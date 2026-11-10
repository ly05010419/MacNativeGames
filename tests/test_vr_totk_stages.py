"""Tears of the Kingdom VR: one picture for both eyes, camera ownership, picture order."""
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == 'Darwin', 'native Mac command-copy allocator')
class TotKStages(unittest.TestCase):
    def test_owned_camera_projection_reaches_every_native_view(self):
        """One picture for both eyes: the union of the two eyes' rays reaches every view of the owned camera."""
        source=(ROOT/'games/totk/runtime/totk_vr_projection.h').read_text()
        source=source[source.index('static pthread_mutex_t'):source.index('void runtime_totk_projection_probe')]
        probe=r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include "vr_projection.h"
#include "vr_view_math.h"
static struct {uint64_t callee,main_vs,cam;}g_vr_cam;
static struct {uint8_t *base;}module,*diag_main=&module;
static uint8_t projection[0xc0];static unsigned calls[3];static float seen[3][7];
static int active=1,calibrated=1;
static const float rays[2][4]={{-.94f,.70f,.77f,-.96f},{-.70f,.94f,.77f,-.96f}};
static int vr_active(void){return active;}
static int runtime_vr_camera_owned(uint64_t camera,uint64_t p){return camera==0xabc&&p==(uintptr_t)projection;}
typedef struct {float fov[2][4];}vr_views;
static int vr_link_views(vr_views *v){if(!calibrated)return 0;memcpy(v->fov,rays,sizeof rays);return 1;}
static void original(uint64_t views,uint32_t index,uint64_t matrix,uint64_t p,uint64_t extra){
 assert(views==0x123&&matrix==0xdef&&p==(uintptr_t)projection&&extra==0x456&&index<3);
 memcpy(seen[index],projection+0xa0,28);calls[index]++;
}
HEADER
int main(void){
 uint64_t vt=0x39023e0;memcpy(projection,&vt,8);float initial[7]={1,2,3,4,5,6,7};memcpy(projection+0xa0,initial,28);
 g_vr_cam.callee=(uintptr_t)original;g_vr_cam.main_vs=0x123;
 /* What the game's projection must be given: both eyes' frusta in one picture, 6% wider. */
 float both[4],target[4];vr_projection_union(rays,both);
 assert(both[0]==-.94f&&both[1]==.94f&&both[2]==.77f&&both[3]==-.96f);
 assert(vr_eye_intrinsics_overscan(both,1.06f,target));
 float want[7]={target[0],sinf(target[0]*.5f),cosf(target[0]*.5f),tanf(target[0]*.5f),target[1],target[2],target[3]};
 for(unsigned i=0;i<3;i++){
  totk_vr_view_set(0x123,i,0xdef,(uintptr_t)projection,0x456,0xabc);
  assert(!memcmp(projection+0xa0,initial,28)); /* the game's own values are back */
  assert(!memcmp(seen[i],want,28));            /* every view of the owned camera got the union */
 }
 assert(g_vr_cam.cam==0xabc&&fabsf(seen[0][5])<1e-6f); /* mirrored eyes: no horizontal shift */
 /* A camera that is not owned passes through unchanged and does not become the main camera. */
 totk_vr_view_set(0x123,1,0xdef,(uintptr_t)projection,0x456,0x999);
 assert(!memcmp(seen[1],initial,28)&&g_vr_cam.cam==0xabc);
 /* No headset calibration: unchanged. */
 calibrated=0;totk_vr_view_set(0x123,2,0xdef,(uintptr_t)projection,0x456,0xabc);
 assert(!memcmp(seen[2],initial,28));calibrated=1;
 /* VR off: unchanged. */
 active=0;totk_vr_view_set(0x123,0,0xdef,(uintptr_t)projection,0x456,0xabc);
 assert(!memcmp(seen[0],initial,28));
 assert(calls[0]==2&&calls[1]==2&&calls[2]==2);return 0;
}
""".replace('HEADER',source)
        with tempfile.TemporaryDirectory(prefix='mng-totk-views-') as folder:
            path=Path(folder)/'probe.c';binary=Path(folder)/'probe';path.write_text(probe)
            result=subprocess.run(['clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-I'+str(ROOT/'zeldaVR/gfx'),'-I'+str(ROOT/'zeldaVR/runtime'),str(path),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_native_camera_ownership_is_not_the_last_writer(self):
        source=(ROOT/'games/totk/runtime/totk_vr_camera_owner.h').read_text().split('__asm__(')[0].replace('#pragma once','')
        probe=r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
static uintptr_t unreadable;
static int jd_read(uint64_t address,void *out,size_t bytes){
    if(address==unreadable)return 0;memcpy(out,(const void *)(uintptr_t)address,bytes);return 1;
}
HEADER
int main(void){
    uint64_t controller_a[4]={0},controller_b[4]={0},holder_a[3]={0,0xa,0x100},holder_b[3]={0,0xb,0x200};
    controller_a[3]=(uintptr_t)holder_a;controller_b[3]=(uintptr_t)holder_b;
    assert(!runtime_vr_camera_owned(0xa,0));
    totk_vr_camera_owner_note((uintptr_t)controller_a,0xa);
    totk_vr_camera_owner_note((uintptr_t)controller_b,0xb);
    /* Both real producers remain owned; scheduling order is not identity. */
    assert(runtime_vr_camera_owned(0xa,0x100)&&runtime_vr_camera_owned(0xb,0x200));
    assert(!runtime_vr_camera_owned(0xa,0x200)&&!runtime_vr_camera_owned(0xc,0));
    for(int i=0;i<100000;i++)totk_vr_camera_owner_note((uintptr_t)controller_b,0xb);
    assert(runtime_vr_camera_owned(0xa,0x100));
    controller_a[3]=0;assert(!runtime_vr_camera_owned(0xa,0));controller_a[3]=(uintptr_t)holder_a;
    holder_a[1]=0xc;assert(!runtime_vr_camera_owned(0xa,0));
    totk_vr_camera_owner_note((uintptr_t)controller_a,0xc);assert(runtime_vr_camera_owned(0xc,0x100));
    unreadable=(uintptr_t)controller_b+0x18;assert(!runtime_vr_camera_owned(0xb,0));
    free(totk_vr_owners);return 0;
}
'''.replace('HEADER',source)
        with tempfile.TemporaryDirectory(prefix='mng-totk-owner-') as folder:
            path=Path(folder)/'probe.c';binary=Path(folder)/'probe';path.write_text(probe)
            result=subprocess.run(['clang','-std=c11','-O1','-Wall','-Wextra','-Werror',str(path),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_newest_picture_follows_present_order(self):
        """The headset shows the newest present in replay order; a late GPU completion of an older one changes nothing."""
        source=(ROOT/'zeldaVR/gfx/totk_vr.m').read_text()
        begin=source.index('static int latest_picture(void){')
        helper=source[begin:source.index('#endif',begin)]
        probe=r"""
#include <assert.h>
#include <stdint.h>
enum {SRC_SLOTS=5};
static struct {uint64_t seq[5],src_order[5];int writing[5];}g;
HELPER
static void image(int slot,uint64_t order,uint64_t sequence){g.src_order[slot]=order;g.seq[slot]=sequence;}
int main(void){
 assert(latest_picture()<0);
 image(0,1,1);assert(latest_picture()==0);
 image(1,2,2);assert(latest_picture()==1);
 image(2,3,0);g.writing[2]=1;assert(latest_picture()==1); /* still being copied */
 g.writing[2]=0;g.seq[2]=3;assert(latest_picture()==2);
 g.seq[0]=100;assert(latest_picture()==2);                /* late completion of an older present */
 return 0;
}
""".replace('HELPER',helper)
        with tempfile.TemporaryDirectory(prefix='mng-totk-order-') as folder:
            path=Path(folder)/'probe.c';binary=Path(folder)/'probe';path.write_text(probe)
            result=subprocess.run(['clang','-std=c11','-O1','-Wall','-Wextra','-Werror',str(path),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(subprocess.run([str(binary)]).returncode,0)

    def test_passdump_line_keeps_its_eye_field(self):
        """Scripts parse "[pass] F<frame> E<eye>": the field is the replay's eye (0 one replay, 1 left, 2 right)."""
        source=(ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        self.assertIn('"[pass] F%llu E%d ", (unsigned long long)atomic_load(&g_cur_frame), g_eye_pass)',source)
