"""Bananza's real copied-argument layout, boundaries and user resolution input."""
import pathlib, shutil, subprocess, tempfile, unittest, sys, platform
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Resolution(unittest.TestCase):
 @unittest.skipUnless(sys.platform=='darwin' and platform.machine()=='arm64','native Apple ARM64 required')
 def test_pokopia_frame_snapshot_preserves_controller(self):
  source=r'''
#include "pokopia_display.h"
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>
__asm__(".text\n.globl _invoke_display\n_invoke_display:\nstp x19,x30,[sp,#-16]!\nmov x16,x0\nmov x0,x1\nblr x16\nldp x19,x30,[sp],#16\nret\n");
extern void invoke_display(void *,void *);
__asm__(".text\n.globl _invoke_camera\n_invoke_camera:\nstp x19,x20,[sp,#-32]!\nstr x30,[sp,#16]\nmov x16,x0\nmov x0,x1\nmov x20,x2\nblr x16\nldr x30,[sp,#16]\nldp x19,x20,[sp],#32\nret\n");
extern void invoke_camera(void *,void *,void *);
int main(void) {
 size_t ps=(size_t)sysconf(_SC_PAGESIZE);
 unsigned char *m=mmap(0,ps*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 if(m==MAP_FAILED)return 1;
 uint64_t *p=(void *)(m+ps);uint32_t code[POKOPIA_DISPLAY_WORDS];
 pokopia_display_code(code,(uintptr_t)m,(uintptr_t)p,(uintptr_t)m+sizeof code);
 memcpy(m,code,sizeof code);uint32_t ret=0xd65f03c0;memcpy(m+sizeof code,&ret,4);
 sys_icache_invalidate(m,sizeof code+4);if(mprotect(m,ps,PROT_READ|PROT_EXEC))return 2;
 unsigned char config[0xe8],before[sizeof config];memset(config,0xa5,sizeof config);
 p[1]=2520ull|(1080ull<<32);invoke_display(m,config);
 if(p[2]!=p[1]||p[3]!=(uintptr_t)config)return 3;
 for(unsigned i=0;i<sizeof config;i++)if(config[i]!=0xa5)return 4;
 config[0x80]=7;memcpy(before,config,sizeof config);invoke_display(m,config);
 if(memcmp(before,config,sizeof config))return 5;
 p[1]=960ull|(540ull<<32);invoke_display(m,config);
 if(p[2]!=p[1]||config[0x80]!=7)return 6;
 if(mprotect(m,ps,PROT_READ|PROT_WRITE))return 7;
 uint32_t resize[POKOPIA_RESIZE_WORDS];pokopia_resize_code(resize,(uintptr_t)m,(uintptr_t)p,(uintptr_t)m+sizeof resize);
 memcpy(m,resize,sizeof resize);memcpy(m+sizeof resize,&ret,4);sys_icache_invalidate(m,sizeof resize+4);
 if(mprotect(m,ps,PROT_READ|PROT_EXEC))return 8;
 uint64_t old=1920ull|(1080ull<<32);memcpy(config+0x48,&old,8);p[4]=old;
 invoke_display(m,config);if(config[0x80]!=7)return 9; /* pending is not committed yet */
 memcpy(config+0x48,&p[2],8);invoke_display(m,config);
 if(config[0x80]!=255||p[4]!=p[2])return 10;
 config[0x80]=7;invoke_display(m,config);if(config[0x80]!=7)return 11;
 unsigned char arg[0xa0],saved[0xa0];memset(arg,0xa5,sizeof arg);arg[2]=0;memcpy(saved,arg,sizeof arg);
 if(pokopia_window_capacity(arg,sizeof arg-1,3840,2160)||memcmp(arg,saved,sizeof arg))return 12;
 if(pokopia_window_capacity(arg,sizeof arg,3841,2160)||memcmp(arg,saved,sizeof arg))return 13;
 if(!pokopia_window_capacity(arg,sizeof arg,3840,2160)||arg[2]!=1)return 14;
 for(unsigned i=0;i<sizeof arg;i++)if(i!=2&&(i<0x94||i>=0x9c)&&arg[i]!=saved[i])return 15;
 unsigned w,h;memcpy(&w,arg+0x94,4);memcpy(&h,arg+0x98,4);if(w!=3840||h!=2160)return 16;
 if(mprotect(m,ps,PROT_READ|PROT_WRITE))return 17;
 uint32_t camcode[POKOPIA_CAMERA_WORDS];
 pokopia_camera_code(camcode,(uintptr_t)m,(uintptr_t)p,(uintptr_t)m+sizeof camcode,(uintptr_t)m+sizeof camcode+4);
 memcpy(m,camcode,sizeof camcode);memcpy(m+sizeof camcode,&ret,4);
 const uint32_t update[]={0xb9016401,0xb9016802,0xd65f03c0}; /* native update's view/projection flags, retained by the fixture */
 memcpy(m+sizeof camcode+4,update,sizeof update);sys_icache_invalidate(m,sizeof camcode+4+sizeof update);
 if(mprotect(m,ps,PROT_READ|PROT_EXEC))return 18;
 unsigned char view[0x2000],camera[0x180];memset(view,0xa5,sizeof view);memset(camera,0xa5,sizeof camera);
 float ratio=16.0f/9;memcpy(camera+0x14c,&ratio,4);p[2]=2520ull|(1080ull<<32);
 invoke_camera(m,view,camera);float vr,cr;memcpy(&vr,view+0x1f20,4);memcpy(&cr,camera+0x14c,4);
 if(vr!=2520.0f/1080||cr!=vr)return 19;
 for(unsigned i=0;i<sizeof view;i++)if((i<0x1f20||i>=0x1f24)&&view[i]!=0xa5)return 20;
 for(unsigned i=0;i<sizeof camera;i++)if((i<0x14c||i>=0x150)&&(i<0x164||i>=0x16c)&&camera[i]!=0xa5)return 21;
 unsigned flags[2];memcpy(flags,camera+0x164,8);if(flags[0]!=1||flags[1]!=1)return 22;
 memset(camera+0x164,0,8);invoke_camera(m,view,camera);memcpy(flags,camera+0x164,8);
 if(flags[0]||flags[1])return 23; /* an unchanged camera does not recompute its matrices */
 if(mprotect(m,ps,PROT_READ|PROT_WRITE))return 24;
 uint32_t ui[POKOPIA_UI_WORDS];pokopia_ui_code(ui,(uintptr_t)m,(uintptr_t)p,(uintptr_t)m+sizeof ui,(uintptr_t)m+sizeof ui+4);
 memcpy(m,ui,sizeof ui);memcpy(m+sizeof ui,&ret,4);
 const uint32_t ui_resize[]={0xf9400030,0xf9018010,0x52800028,0xb9030808,0xd65f03c0};
 memcpy(m+sizeof ui+4,ui_resize,sizeof ui_resize);sys_icache_invalidate(m,sizeof ui+4+sizeof ui_resize);
 if(mprotect(m,ps,PROT_READ|PROT_EXEC))return 25;
 unsigned char slot[0x310];memset(slot,0xa5,sizeof slot);uint64_t logical=1920ull|(1080ull<<32);memcpy(slot+0x300,&logical,8);
 p[2]=2520ull|(1080ull<<32);invoke_display(m,slot);memcpy(&logical,slot+0x300,8);
 unsigned called;memcpy(&called,slot+0x308,4);if(logical!=p[2]||called!=1)return 26;
 for(unsigned i=0;i<sizeof slot;i++)if((i<0x300||i>=0x30c)&&slot[i]!=0xa5)return 27;
 memset(slot+0x308,0,4);invoke_display(m,slot);memcpy(&called,slot+0x308,4);if(called)return 28;
 p[2]=960ull|(540ull<<32);invoke_display(m,slot);memcpy(&logical,slot+0x300,8);
 if(logical!=(1920ull|(1080ull<<32)))return 29;

 return munmap(m,ps*2)!=0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   c=pathlib.Path(d)/'live.c';exe=pathlib.Path(d)/'live';c.write_text(source)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'games/totk/runtime'),str(c),'-o',str(exe)],check=True,capture_output=True)
   result=subprocess.run([str(exe)],capture_output=True,text=True)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
 @unittest.skipUnless(sys.platform=='darwin' and platform.machine()=='arm64','native Apple ARM64 required')
 def test_live_display_runs_on_game_thread(self):
  source=r'''
#include "bananza_display.h"
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>
#include <stdlib.h>
__asm__(".text\n.globl _invoke_registered\n_invoke_registered:\nstp x29,x30,[sp,#-16]!\nmov x16,x0\nmov x8,x1\nmov x0,x2\nblr x16\nldp x29,x30,[sp],#16\nret\n");
extern void invoke_registered(void *,void *,void *);
int main(void) {
 size_t ps=(size_t)sysconf(_SC_PAGESIZE);
 unsigned char *m=mmap(0,ps*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 if(m==MAP_FAILED)return 1;
 uint64_t *p=(void *)(m+ps);uint32_t code[BANANZA_DISPLAY_WORDS];
 bananza_display_code(code,(uintptr_t)m,(uintptr_t)p,(uintptr_t)m+sizeof code);
 memcpy(m,code,sizeof code);uint32_t ret=0xd65f03c0;memcpy(m+sizeof code,&ret,4);
 sys_icache_invalidate(m,sizeof code+4);
 if(mprotect(m,ps,PROT_READ|PROT_EXEC))return 2;
 unsigned char config[0x230],before[sizeof config];memset(config,0xa5,sizeof config);
 uint64_t wanted=1920ull|(1080ull<<32);p[0]=3840ull|(2160ull<<32);p[1]=wanted;
 ((void(*)(void *))m)(config);
 if(p[2]!=wanted||p[3]!=(uintptr_t)config)return 3;
 for(unsigned i=0;i<sizeof config;i++){
  if((i>=0x10&&i<0x18)||(i>=0xd4&&i<0xdc)||(i>=0x138&&i<0x140)||(i>=0x1a0&&i<0x1a8))continue;
  if(i==8){if(config[i]!=(0xa5|0x50))return 4;}
  else if(i==0x1c5){if(config[i]!=255)return 5;}
  else if(config[i]!=0xa5)return 6;
 }
 const unsigned off[]={0x10,0xd4,0x138,0x1a0};
 for(unsigned i=0;i<4;i++){uint64_t v;memcpy(&v,config+off[i],8);if(v!=wanted)return 7;}
 memcpy(before,config,sizeof config);((void(*)(void *))m)(config);
 if(memcmp(config,before,sizeof config))return 8; /* repeated frames do not reflag */
 p[1]=3840ull|(2160ull<<32);p[1]=960ull|(540ull<<32); /* latest pending selection wins */
 ((void(*)(void *))m)(config);
 for(unsigned i=0;i<4;i++){uint64_t v;memcpy(&v,config+off[i],8);if(v!=p[1])return 9;}
 if(mprotect(m,ps,PROT_READ|PROT_WRITE))return 10;
 uint32_t camcode[BANANZA_CAMERA_WORDS];
 bananza_camera_code(camcode,(uintptr_t)m,(uintptr_t)p,(uintptr_t)m+sizeof camcode,(uintptr_t)m+sizeof camcode+4);
 memcpy(m,camcode,sizeof camcode);memcpy(m+sizeof camcode,&ret,4);
 const uint32_t update[]={0x52800030,0xb900c010,0xd65f03c0}; /* fake native update records that it ran */
 memcpy(m+sizeof camcode+4,update,sizeof update);sys_icache_invalidate(m,sizeof camcode+4+sizeof update);
 if(mprotect(m,ps,PROT_READ|PROT_EXEC))return 11;
 unsigned char camera[0x200],target[32],saved[32];memset(camera,0xa5,sizeof camera);memset(target,0x5a,sizeof target);memcpy(saved,target,sizeof saved);
 camera[8]=0x40;float ratio=16.0f/9;memcpy(camera+0xb0,&ratio,4);
 uint64_t holder[3]={0,0,(uintptr_t)camera};p[2]=2520ull|(1080ull<<32);
 invoke_registered(m,holder,target);memcpy(&ratio,camera+0xb0,4);
 unsigned called;memcpy(&called,camera+0xc0,4);
 if(ratio!=2520.0f/1080||called!=1||camera[8]!=0x41||memcmp(target,saved,sizeof target))return 12;
 for(unsigned i=0;i<sizeof camera;i++)if(i!=8&&(i<0xb0||i>=0xb4)&&(i<0xc0||i>=0xc4)&&camera[i]!=0xa5)return 13;
 memset(camera+0xc0,0,4);invoke_registered(m,holder,target);memcpy(&called,camera+0xc0,4);if(called)return 14;
 return munmap(m,ps*2)!=0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   c=pathlib.Path(d)/'live.c';exe=pathlib.Path(d)/'live';c.write_text(source)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'games/totk/runtime'),str(c),'-o',str(exe)],check=True,capture_output=True)
   result=subprocess.run([str(exe)],capture_output=True,text=True)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
 def test_native_argument_and_input(self):
  compiler=shutil.which('cc')
  if not compiler:self.skipTest('C compiler required')
  source=r'''
#include "ns2_resolution.h"
#include <string.h>
int main(void) {
 const char *valid[]={"1280x720","960x540","1000x600","16384x16384","1x1"};
 const char *bad[]={"","720p","0x720","1280x0","-1x720","1x-2","1x2junk","1x2x3"," 1280x720","16385x720","999999999999999999999999999999x720"};
 for(unsigned i=0;i<sizeof valid/sizeof *valid;i++){unsigned w=0,h=0;if(!ns2_parse_resolution(valid[i],&w,&h)||!w||!h)return 1;}
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){unsigned w=13,h=17;if(ns2_parse_resolution(bad[i],&w,&h)||w!=13||h!=17)return 2;}
 unsigned aw,ah;
 if(!ns2_aspect_size(32.0f/9,1080,3840,2160,&aw,&ah)||aw!=3840||ah!=1080)return 21;
 if(!ns2_aspect_size(32.0f/9,2160,3840,2160,&aw,&ah)||aw!=3840||ah!=1080)return 22;
 if(!ns2_aspect_size(16.0f/9,2160,3840,2160,&aw,&ah)||aw!=3840||ah!=2160)return 23;
 if(!ns2_aspect_size(21.0f/9,1080,3840,2160,&aw,&ah)||aw!=2520||ah!=1080)return 24;
 bananza_gsys_sizes p;memset(&p,0xa5,sizeof p);p.extent[0].w=960;p.extent[0].h=540;
 bananza_gsys_sizes before=p;
 if(bananza_set_gsys_sizes(&p,sizeof p-8,960,540)||memcmp(&p,&before,sizeof p))return 3;
 if(bananza_set_gsys_sizes(&p,sizeof p,1280,720)||memcmp(&p,&before,sizeof p))return 4;
 if(!bananza_set_gsys_sizes(&p,sizeof p,960,540))return 5;
 if(p.flags!=before.flags||memcmp(p.tail,before.tail,sizeof p.tail))return 6;
 for(unsigned i=0;i<24;i++)if(p.extent[i].w!=960||p.extent[i].h!=540)return 7;
 for(unsigned i=0;i<6;i++)if(p.scale[i].x!=1||p.scale[i].y!=1)return 8;
 unsigned char memory[0x11d],saved[0x11d];memset(memory,0xa5,sizeof memory);
 uint64_t pool=200ULL<<20;memcpy(memory+0x20,&pool,8);memcpy(saved,memory,sizeof memory);
 if(bananza_render_pool_bytes(1920,1080)!=pool||bananza_render_pool_bytes(960,540)!=pool)return 9;
 if(bananza_render_pool_bytes(3840,2160)!=(800ULL<<20))return 10;
 if(bananza_render_pool_bytes(2560,1440)!=(364096ULL<<10))return 20;
 if(bananza_render_pool_bytes(16384,16384)<(200ULL<<20))return 11;
 if(bananza_set_render_pool(memory,sizeof memory-1,3840,2160)||memcmp(memory,saved,sizeof memory))return 12;
 if(!bananza_set_render_pool(memory,sizeof memory,1920,1080)||memcmp(memory,saved,sizeof memory))return 13;
 if(!bananza_set_render_pool(memory,sizeof memory,3840,2160))return 14;
 memcpy(&pool,memory+0x20,8);if(pool!=(800ULL<<20))return 15;
 memcpy(memory+0x20,saved+0x20,8);if(memcmp(memory,saved,sizeof memory))return 16;
 pool=1;memcpy(memory+0x20,&pool,8);memcpy(saved,memory,sizeof memory);
 if(bananza_set_render_pool(memory,sizeof memory,3840,2160)||memcmp(memory,saved,sizeof memory))return 17;
 unsigned sw=0,sh=0;
 if(!ns2_parse_saved_resolution("3840x2160\n",&sw,&sh)||sw!=3840||sh!=2160)return 18;
 if(ns2_parse_saved_resolution("3840x2160x\n",&sw,&sh))return 19;
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   c=pathlib.Path(d)/'probe.c';exe=pathlib.Path(d)/'probe';c.write_text(source)
   subprocess.run([compiler,'-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'games/totk/runtime'),str(c),'-o',str(exe)],check=True,capture_output=True)
   p=subprocess.run([str(exe)],capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
 def test_phone_pane_uses_engine_parameter(self):
  import importlib.util, plistlib
  path=ROOT/'games/totk/ios/tools';sys.path.insert(0,str(path))
  import build_ipa
  root=build_ipa.bananza_pane(plistlib.loads((ROOT/'games/totk/ios/Settings.bundle/Root.plist').read_bytes()))
  rows={r['Key']:r for r in root['PreferenceSpecifiers'] if 'Key' in r}
  self.assertEqual(rows['TOTK_OUTPUT_RES']['DefaultValue'],'960x540')
  self.assertEqual(rows['TOTK_OUTPUT_RES']['Type'],'PSTextFieldSpecifier')
  self.assertNotIn('TOTK_DOCKED',rows)
  self.assertNotIn('TOTK_RENDER_RES',rows)
