"""Shared attachment identity: duplicate storage, mip/slice separation, clears and prediction."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RenderTargetDuplicates(unittest.TestCase):
    def test_production_identity_and_clear_routing_for_both_games(self):
        gfx=ROOT/'games/totk/third_party/d2r_gfx/gfx'
        source=r'''
#include "nvn_obj.h"
#include <assert.h>
#include <stdio.h>
static struct {p_set_rt rt;int clear_c[8];struct{float x,y,z,w;}cc[8];}R;
static void clear(p_clear_color *c){NVN_RT_DUP_CLEAR_TO_FIRST(c) R.clear_c[c->index]=1;R.cc[c->index].x=c->rgba[0];}
int main(void){
    tex_t a={0},b={0};a.serial=1;a.mem=(void *)0x100000;a.L.size=320*180*4;
    a.d.width=320;a.d.height=180;a.d.depth=1;a.d.levels=2;a.d.target=1;a.d.samples=1;a.d.format=NVN_FORMAT_RGBA8;
    b=a;b.serial=2;R.rt.n=2;R.rt.color[0]=&a;R.rt.color[1]=&b;
    assert(rt_dup_of(&R.rt,1)==0); /* separate descriptors, same actual pixels */
    uint32_t formats[8]={70,70};NVN_RT_DUP_PREDICT(&R.rt,formats);assert(formats[0]==70&&!formats[1]);
    p_clear_color c={.index=1,.mask=15,.rgba={.25,0,0,1}};clear(&c);assert(R.clear_c[0]&&!R.clear_c[1]&&R.cc[0].x==.25f);
    c.index=0;c.rgba[0]=.5f;clear(&c);assert(R.cc[0].x==.5f);
    R.rt.has_cview[1]=1;R.rt.cview[1].set_layers=1;R.rt.cview[1].min_layer=1;assert(rt_dup_of(&R.rt,1)<0);
    R.rt.cview[1].min_layer=0;R.rt.cview[1].set_levels=1;R.rt.cview[1].base_level=1;assert(rt_dup_of(&R.rt,1)<0);
    R.rt.cview[1].base_level=0;assert(rt_dup_of(&R.rt,1)==0);
    b.mem=(void *)0x200000;assert(rt_dup_of(&R.rt,1)<0);b.mem=a.mem;
    b.d.width=160;assert(rt_dup_of(&R.rt,1)<0);b.d.width=320;
    b.L.size--;assert(rt_dup_of(&R.rt,1)<0);
    puts("shared RT duplicate identity, views, prediction and ordered clears passed");
}
'''
        with tempfile.TemporaryDirectory(prefix='mng-rt-duplicates-') as folder:
            path=Path(folder)/'probe.c';path.write_text(source)
            for botw in (False,True):
                binary=Path(folder)/('botw' if botw else 'totk')
                args=['clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(gfx/'nvn'),'-I'+str(gfx/'nvn/include')]
                if botw:args+=['-DGAME_BOTW=1']
                result=subprocess.run(args+[str(path),str(gfx/'tex/nvn_fmt_mtl.c'),str(gfx/'nvn/nvn_tex.c'),'-o',str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                result=subprocess.run([str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
