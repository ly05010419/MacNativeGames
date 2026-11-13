"""Native production-scheduler checks for opt-in mutex-chain inheritance."""
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(sys.platform=='darwin' and platform.machine()=='arm64','native Apple scheduler')
class PriorityChain(unittest.TestCase):
    def test_query_core_grant_cycles_handoffs_and_defaults(self):
        with tempfile.TemporaryDirectory() as d:
            binary=str(Path(d)/'priority-chain')
            compile=subprocess.run(['xcrun','clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=undefined','-fno-sanitize-recover=all','-DGAME_BOTW=1',
                '-I'+str(ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn/include'),'-Wl,-dead_strip',
                str(ROOT/'tests/test_botw_priority_chain.c'),'-o',binary],capture_output=True,text=True,timeout=60)
            self.assertEqual(compile.returncode,0,compile.stdout+compile.stderr)
            for mode in ('0','1','2'):
                with self.subTest(mode=mode):
                    result=subprocess.run([binary,mode],capture_output=True,text=True,timeout=15)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=='__main__':unittest.main()
