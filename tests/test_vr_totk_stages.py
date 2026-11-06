"""Production TotK stage pairing and command-copy ownership across arbitrary submits."""
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
        source=(ROOT/'games/totk/runtime/totk_vr_projection.h').read_text()
        source=source[source.index('static pthread_mutex_t'):source.index('void runtime_totk_projection_probe')]
        probe=r'''
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include "vr_projection.h"
#include "vr_view_math.h"
static struct {uint64_t callee,main_vs,cam;}g_vr_cam;
static struct {unsigned on,phase;}g_vr_b2={1,0};
static struct {uint8_t *base;}module,*diag_main=&module;
static uint8_t projection[0xc0];static unsigned calls[3];static float seen[2][3][7];
static int vr_active(void){return 1;}
static int runtime_vr_camera_owned(uint64_t camera,uint64_t p){return camera==0xabc&&p==(uintptr_t)projection;}
typedef struct {float fov[2][4];}vr_views;
static int vr_link_views(vr_views *v){
 const float rays[2][4]={{-.94f,.70f,.77f,-.96f},{-.70f,.94f,.77f,-.96f}};memcpy(v->fov,rays,sizeof rays);return 1;
}
static void original(uint64_t views,uint32_t index,uint64_t matrix,uint64_t p,uint64_t extra){
 assert(views==0x123&&matrix==0xdef&&p==(uintptr_t)projection&&extra==0x456&&index<3);
 memcpy(seen[g_vr_b2.phase][index],projection+0xa0,28);calls[index]++;
}
HEADER
int main(void){
 uint64_t vt=0x39023e0;memcpy(projection,&vt,8);float initial[7]={1,2,3,4,5,6,7};memcpy(projection+0xa0,initial,28);
 g_vr_cam.callee=(uintptr_t)original;g_vr_cam.main_vs=0x123;
 for(unsigned eye=0;eye<2;eye++){
  g_vr_b2.phase=eye;
  for(unsigned i=0;i<3;i++){
   totk_vr_view_set(0x123,i,0xdef,(uintptr_t)projection,0x456,0xabc);
   assert(!memcmp(projection+0xa0,initial,28));
  }
  assert(!memcmp(seen[eye][0],seen[eye][1],28)&&!memcmp(seen[eye][0],seen[eye][2],28));
  assert(memcmp(seen[eye][0],initial,28));
 }
 assert(seen[0][0][5]*seen[1][0][5]<0);
 /* A different camera passed through the same setter must remain untouched. */
 totk_vr_view_set(0x123,1,0xdef,(uintptr_t)projection,0x456,0x999);
 assert(!memcmp(seen[1][1],initial,28)&&g_vr_cam.cam==0xabc);
 assert(calls[0]==2&&calls[1]==3&&calls[2]==2);return 0;
}
'''.replace('HEADER',source)
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

    def test_render_identity_and_retained_copy_lifetime(self):
        source=(ROOT/'games/totk/runtime/game_patches_v143.c').read_text()
        hle=(ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c').read_text()
        prefix=re.search(r'typedef struct \{ [^\n]*vr_record[^\n]*\} copy_prefix;',hle).group()
        begin=hle.index('void hle_vr_copy_tag(')
        owners=hle[begin:hle.index('#endif',begin)]
        begin=hle.index('static void copy_free(')
        retire=hle[begin:hle.index('static uint64_t g_copy_drop_readd',begin)]
        begin=hle.index('static int copy_kept(const cmd_copy *c) {')
        kept=hle[begin:hle.index('static void release_copies(',begin)]
        begin=source.index('static pthread_mutex_t b2_stage_mu=')
        records=source[begin:source.index('__asm__(',begin)]
        begin=source.index('void b2_between_runs(void){')
        capture=source[begin:source.index('static uint64_t b2_rec_key(',begin)]
        begin=source.index('void b2_after_runs(void){')
        pair=source[begin:source.index('/* By the recording\'s own frame',begin)]
        begin=source.index('uint64_t runtime_b2_twin(')
        twin=source[begin:source.index('void runtime_b2_called(',begin)]
        probe=r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc/malloc.h>
typedef uintptr_t NVNcommandHandle;
#define COPY_MAGIC 0x59504f43444d43ull
#define die(...) do{fprintf(stderr,__VA_ARGS__);exit(2);}while(0)
#define runtime_fail(...) die(__VA_ARGS__)
PREFIX
static copy_prefix *copy_of(NVNcommandHandle h){return (copy_prefix *)h-1;}
OWNERS
static _Atomic uint64_t g_copy_bytes;
static int async_replay(void){return 0;}
static uint64_t xsub_pushed(void){return 0;}
static uint64_t xsub_replayed(void){return 0;}
typedef struct {copy_prefix *pf;uint64_t after;}retired_copy;
static retired_copy *g_retired;static size_t g_nretired,g_cretired;
RETIRE
typedef struct {uint8_t *copy;}cmd_copy;
KEPT
static struct {uint32_t on,mask,runs;}g_vr_b2={3,0x181,40};
static _Atomic uint64_t b2_why[4],b2_twin_hits,b2_twin_miss,b2_marks,b2_slot_hits,b2_map_n;
static _Atomic uint32_t b2_twin_frame,b2_cap_frame;
static void *_Atomic b2_left_cap;
static unsigned captured;
static void b2_stage1_run1(void){}
static void *mtl_sub_capture_ubos(const uint64_t *handles,int count){
    for(int i=0;i<count;i++)assert(copy_of(handles[i])->magic==COPY_MAGIC&&copy_of(handles[i])->vr_refs);
    captured=(unsigned)count;return malloc(1);
}
static void mtl_ucap_free(void *p){free(p);}
RECORDS
CAPTURE
PAIR
TWIN
static uint64_t record(uint64_t owner,uint64_t layer,int phase,uint32_t frame){
    copy_prefix *p=calloc(1,sizeof *p+16);p->magic=COPY_MAGIC;g_copy_bytes+=malloc_size(p);
    uint64_t h=(uint64_t)(uintptr_t)(p+1);hle_vr_copy_tag(h,frame,(unsigned)phase);b2_stage_record(h,owner,layer);return h;
}
static int held(uint64_t h){cmd_copy copy={(uint8_t *)(uintptr_t)h};return copy_kept(&copy);}
static void retire(uint64_t h){assert(!held(h));copy_prefix *p=copy_of(h);p->magic=0;copy_free(p);}
int main(void){
    uint64_t left_a=record(0xa,0x10,0,40),left_b=record(0xa,0x20,0,40);
    assert(held(left_a)&&held(left_b));b2_between_runs();assert(captured==2);
    uint64_t right_b=record(0xa,0x20,1,40),right_a=record(0xa,0x10,1,40);
    assert(!runtime_b2_twin(right_a)); /* no pairing before the actual stage join */
    b2_after_runs();assert(!held(right_a)&&held(left_a));
    g_vr_b2.runs=1000000; /* elapsed submits/frames do not end actual ownership */
    assert(runtime_b2_twin(right_a)==left_a&&runtime_b2_twin(right_b)==left_b);
    retire(right_a);assert(!held(left_a)&&held(left_b));retire(left_a);
    retire(right_b);assert(!held(left_b));retire(left_b);
    g_vr_b2.runs=41;
    uint64_t foreign=record(0xb,0x10,0,41),unmatched=record(0xa,0x10,1,41);
    b2_after_runs();assert(!runtime_b2_twin(unmatched)&&!held(foreign));retire(foreign);retire(unmatched);
    g_vr_b2.runs=42;
    uint64_t one=record(0xc,0x30,0,42),two=record(0xc,0x30,0,42),ambiguous=record(0xc,0x30,1,42);
    b2_after_runs();assert(!runtime_b2_twin(ambiguous));assert(!held(one)&&!held(two));retire(one);retire(two);retire(ambiguous);
    free(atomic_exchange(&b2_left_cap,NULL));assert(g_copy_bytes==0);
    puts("TotK stage identity and owned copy lifetime passed");
}
'''
        for name,value in [('PREFIX',prefix),('OWNERS',owners),('RETIRE',retire),('KEPT',kept),('RECORDS',records),('CAPTURE',capture),('PAIR',pair),('TWIN',twin)]:
            probe=probe.replace(name,value)
        with tempfile.TemporaryDirectory(prefix='mng-totk-stages-') as folder:
            path=Path(folder)/'probe.c';binary=Path(folder)/'probe';path.write_text(probe)
            result=subprocess.run(['clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-variable',str(path),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_eye_images_publish_only_complete_submission_pairs(self):
        source=(ROOT/'zeldaVR/gfx/totk_vr.m').read_text()
        begin=source.index('static int latest_pair(')
        helper=source[begin:source.index('static void start_thread(',begin)]
        probe=r'''
#include <assert.h>
#include <stdint.h>
enum {SRC_SLOTS=5};
static struct {uint64_t seq[5],src_pair[5],src_order[5];int writing[5],src_eye[5],src_mapped[5];}g;
HELPER
static void image(int slot,int eye,uint64_t pair,uint64_t sequence){g.src_eye[slot]=eye;g.src_pair[slot]=pair;g.src_order[slot]=pair;g.seq[slot]=sequence;g.src_mapped[slot]=1;}
int main(void){
 int p[2];assert(!latest_pair(p));
 image(0,0,1,1);image(1,1,1,2);assert(latest_pair(p)&&p[0]==0&&p[1]==1);
 image(2,0,2,3);assert(latest_pair(p)&&p[0]==0&&p[1]==1);
 image(3,1,2,4);g.writing[3]=1;assert(latest_pair(p)&&p[0]==0&&p[1]==1);
 g.writing[3]=0;assert(latest_pair(p)&&p[0]==2&&p[1]==3);
 image(4,0,3,5);assert(latest_pair(p)&&p[0]==2&&p[1]==3);
 g.seq[1]=100;assert(latest_pair(p)&&p[0]==2&&p[1]==3); /* late old completion */
 /* F7209: one menu eye had projection metadata and the other did not.
    Both eyes still belong to the same menu packet. Do not fall back to
    the old world when the second eye completes, then flip on the next eye. */
 g.src_mapped[3]=0;assert(latest_pair(p)&&p[0]==2&&p[1]==3);
 assert(latest_picture(p)==2&&p[0]==2&&p[1]==3);
 assert(totk_picture_sources(p)==1&&p[0]==3&&p[1]==3); /* final present owns every flat-menu pixel */
 g.src_mapped[3]=1;assert(totk_picture_sources(p)==2&&p[0]==2&&p[1]==3);g.src_mapped[3]=0;
 /* An incomplete newer pair cannot publish half a menu, even after a slow
    GPU frame. Only the completed pair supersedes the current packet. */
 assert(g.src_pair[4]==3);assert(latest_picture(p)==2&&p[0]==2&&p[1]==3);
 image(0,1,3,101);g.src_mapped[0]=0;assert(latest_picture(p)==2&&p[0]==4&&p[1]==0);
 assert(totk_picture_sources(p)==1&&p[0]==0&&p[1]==0);
 /* A real ordinary present supersedes it in replay order. Late callbacks
    from an older pair cannot resurrect the old world. */
 image(1,-1,0,102);g.src_order[1]=4;
 assert(latest_picture(p)==1&&p[0]==1&&p[1]==1);
 assert(totk_picture_sources(p)==1&&p[0]==1&&p[1]==1);
 g.seq[0]=200;g.seq[4]=201;assert(latest_picture(p)==1&&p[0]==1);
 image(2,0,5,202);assert(latest_picture(p)==1&&p[0]==1);
 image(3,1,5,203);assert(latest_picture(p)==2&&p[0]==2&&p[1]==3);
}
'''.replace('HELPER',helper)
        with tempfile.TemporaryDirectory(prefix='mng-totk-pairs-') as folder:
            path=Path(folder)/'probe.c';binary=Path(folder)/'probe';path.write_text(probe)
            result=subprocess.run(['clang','-std=c11','-O1','-Wall','-Wextra','-Werror',str(path),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(subprocess.run([str(binary)]).returncode,0)
