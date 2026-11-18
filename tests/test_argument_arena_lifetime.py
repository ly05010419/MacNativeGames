"""#2113: distinguish leaving residency from deallocation with inactive shaders alive."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'games/totk/scripts'))
from dev.game_slot import GameSlot

@unittest.skipUnless(sys.platform=='darwin','requires Metal 4')
class ArenaLifetime(unittest.TestCase):
    def test_production_lifetime_and_negative_controls(self):
        src=(ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        field=next(l for l in src.splitlines() if 'id<MTLBuffer> ab_buf;' in l)
        start=src.index('@interface NvArena :')
        arena=src[start:src.index('\n//',src.index('static size_t arena_alloc(size_t n, __strong',start))]
        idle=re.search(r'^#define STOR_IDLE_SUBMITS \d+',src,re.M)[0]
        template=(ROOT/'tests/argument_arena_lifetime_probe.m').read_text()
        source=template.replace('// SHADER_FIELD','@interface NvShader : NSObject { @public\n'+field+'\n} @end\n@implementation NvShader @end')
        source=source.replace('// ARENA_IMPLEMENTATION',idle+'\n'+arena)
        expression=re.search(r'    int reuse = (.*?);\n    int verify_epoch',src,re.S)[1]
        wiring='''
static struct { uint64_t serial; } g_pass={1};
static int g_args_dirty[6],g_vr_collapse_draw,g_vr_ui_ray_draw;
static int shader_uses_pool(const NvShader *sh) {(void)sh;return 0;}
static int passdump(void) {return 0;}
static int production_reuse(NvShader *sh,int epoch_mode) {
    int ab_noreuse=0,nvn_stage=0,epoch_hit=1;
    return EXPRESSION;
}
'''.replace('EXPRESSION',expression)
        source=source.replace('static void require(',wiring+'\nstatic void require(')
        source=source.replace('[shaders addObject:sh];','sh->ab_pass=1; require(production_reuse(sh,0),"production same-pass reuse"); [shaders addObject:sh];')
        source=source.replace('require(!shader_ab_current(sh),"new submission rejects old cache even with unchanged pass")',
            'require(!production_reuse(sh,0)&&!production_reuse(sh,1),"new submission rejects old cache even with unchanged pass")')
        with tempfile.TemporaryDirectory() as tmp:
            work=Path(tmp)
            built=[]
            for mode in ('fixed','old-ownership','old-generation'):
                code=source
                if mode=='old-ownership':code=code.replace('__unsafe_unretained id<MTLBuffer> ab_buf;','id<MTLBuffer> ab_buf;')
                if mode=='old-generation':code=code.replace('sh->ab_arena == g_arena_epoch && sh->ab_buf != nil','sh->ab_buf != nil')
                file=work/(mode+'.m');binary=work/mode;file.write_text(code)
                subprocess.run(['xcrun','clang','-std=gnu11','-O1','-fobjc-arc','-fsanitize=address,undefined',str(file),'-framework','Foundation','-framework','Metal','-o',str(binary)],check=True)
                built.append((mode,binary))
            with GameSlot(owner='tests',purpose='argument arena lifetime and negative controls',expect_seconds=90) as slot:
                for mode,binary in built:
                    p=slot.popen([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                    try:out,err=p.communicate(timeout=30)
                    finally:
                        if p.poll() is None:p.kill();p.wait()
                    if p.returncode==77:self.skipTest('Metal 4 unavailable')
                    if mode=='fixed':
                        self.assertEqual(p.returncode,0,out+err);self.assertIn('128 MiB deallocated',out)
                    else:
                        self.assertEqual(p.returncode,1,out+err)
                        self.assertIn('inactive shaders must not retain' if mode=='old-ownership' else 'new submission rejects old cache',err)
                    print(mode,out.strip(),err.strip())

if __name__=='__main__':unittest.main()
