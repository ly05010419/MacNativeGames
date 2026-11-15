"""Real GPU HMMA output with and without callable helper preservation."""
import ast
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
class HmmaCalls(unittest.TestCase):
    def test_numerical_identity_and_chained_simd_barriers(self):
        # Reuse the project's independently reconstructed PTX matrix layout.
        tree=ast.parse((ROOT/'games/ss/runtime/gfx/tests/test_dmsh_hmma.py').read_text())
        values={n.targets[0].id:ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign)
                and isinstance(n.targets[0],ast.Name) and n.targets[0].id in ('HEAD','HELPER','KERNEL')}
        source=(ROOT/'games/oot/tools/totk_xlate/TensorMsl.cs').read_text()
        body=re.search(r'const string Body = """\n(.*?)\n""";',source,re.S)[1]
        qualifier=re.search(r'CallQualifier = "([^"]+)"',source)[1]
        names=[f'_{n}' for n in range(42961,42972)]
        for key,name in zip(('lane','blk','a0','a1','a2','a3','b0','b1','acc0','acc1','d1'),names):body=body.replace('%('+key+')s',name)
        body=body.replace('%(scr)s','_34').replace('%(rt)s','int').replace('%(dt)s','int')
        signature=values['HELPER'].split('\n{')[0]
        kernel=values['KERNEL'].replace('uint gid [[thread_position_in_grid]])','uint gid [[thread_position_in_grid]], constant uint& repetitions [[buffer(2)]])')
        old='int d0 = dmsh_hmma16816_f16(l, blk, a0, a1, a2, a3, b0, b1, c0, c1, d1, scr);'
        new='int d0 = 0; for(uint iteration=0; iteration<repetitions; iteration++) { d0 = dmsh_hmma16816_f16(l, blk, a0, a1, a2, a3, b0, b1, c0, c1, d1, scr); c0=d0; c1=d1; }'
        self.assertIn(old,kernel);kernel=kernel.replace(old,new)
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory);libraries={}
            for name,attr,text in [('inline','static inline __attribute__((always_inline))',body),('call',qualifier,body),
                                   ('wrong-result',qualifier,body.replace('return as_type<int>(d0);','return as_type<int>(d0) ^ 1;'))]:
                file,library=work/(name+'.metal'),work/(name+'.metallib')
                file.write_text(values['HEAD']+'\n'+attr+signature+'\n'+text+'\n'+kernel)
                subprocess.run(['xcrun','-sdk','macosx','metal','-fmetal-math-mode=safe','-fpreserve-invariance','-fwrapv','-c',str(file),'-o',str(work/(name+'.air'))],check=True)
                subprocess.run(['xcrun','-sdk','macosx','metallib',str(work/(name+'.air')),'-o',str(library)],check=True)
                if name!='wrong-result':
                    # The same helper is emitted for mobile; compile the real
                    # body for the app's minimum iOS version as well.
                    ios_air=work/(name+'-ios.air')
                    subprocess.run(['xcrun','-sdk','iphoneos','metal','-mtargetos=ios26.0',
                                    '-fmetal-math-mode=safe','-fpreserve-invariance','-fwrapv',
                                    '-c',str(file),'-o',str(ios_air)],check=True)
                    subprocess.run(['xcrun','-sdk','iphoneos','metallib',str(ios_air),
                                    '-o',str(work/(name+'-ios.metallib'))],check=True)
                libraries[name]=library
            binary=work/'probe'
            subprocess.run(['xcrun','clang','-O1','-fobjc-arc',str(ROOT/'tests/hmma_calls_probe.m'),'-framework','Foundation','-framework','Metal','-o',str(binary)],check=True)
            with GameSlot(owner='tests',purpose='HMMA call GPU numerical identity',expect_seconds=60) as slot:
                for name in ('call','wrong-result'):
                    p=slot.popen([str(binary),str(libraries['inline']),str(libraries[name])],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                    try:out,err=p.communicate(timeout=25)
                    finally:
                        if p.poll() is None:p.kill();p.wait(timeout=5)
                    if p.returncode==77:self.skipTest('requires Metal 4')
                    self.assertEqual(p.returncode,0 if name=='call' else 1,out+err)
                    self.assertIn('65536 half outputs bit-identical' if name=='call' else 'function-call rewrite changed GPU matrix output',out+err)
                    print(name,out.strip(),err.strip())


if __name__=='__main__':unittest.main()
