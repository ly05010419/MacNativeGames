"""Bananza's real copied-argument layout, boundaries and user resolution input."""
import pathlib, shutil, subprocess, tempfile, unittest, sys
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Resolution(unittest.TestCase):
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
