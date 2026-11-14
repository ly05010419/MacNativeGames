"""Run the actual scaled-copy implementation on Metal 4 with independent pixel expectations."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'games/totk/scripts'))
from dev.game_slot import GameSlot

@unittest.skipUnless(sys.platform=='darwin','requires Metal 4')
class TextureCopies(unittest.TestCase):
    def test_production_copies(self):
        nvn=ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'
        src=(nvn/'nvn_mtl.m').read_text()
        functions=src[src.index('static int t2t_fmt_int_or_ds('):src.index('static void copy_t2t(const p_copy_t2t *c) {')]
        with tempfile.TemporaryDirectory() as tmp:
            work=Path(tmp); probe=work/'probe.m'; binary=work/'probe'
            probe.write_text((ROOT/'tests/texture_copy_gpu_probe.m').read_text().replace('// PRODUCTION_COPY',functions))
            subprocess.run(['xcrun','clang','-std=gnu11','-O1','-fobjc-arc','-I'+str(nvn/'include'),str(probe),
                '-framework','Metal','-framework','Foundation','-o',str(binary)],check=True)
            with GameSlot(owner='tests',purpose='scaled texture copy pixels',expect_seconds=60) as slot:
                p=slot.popen([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                try: out,err=p.communicate(timeout=60)
                finally:
                    if p.poll() is None:p.kill();p.wait()
                if p.returncode==77:self.skipTest('Metal 4 unavailable')
                self.assertEqual(p.returncode,0,out+err)
                self.assertIn('16 production texture-copy GPU cases',out)
                print(out.strip())

if __name__=='__main__':unittest.main()
