"""The reported resolution-expanded pool must fit inside the virtual heap reservation."""
from pathlib import Path
import shutil, subprocess, tempfile, unittest
ROOT = Path(__file__).resolve().parents[1]
@unittest.skipUnless(shutil.which('clang'), 'clang is required')
class OutputMemory(unittest.TestCase):
    def test_reported_pool_fits_the_virtual_heap(self):
        code = r'''
#include "output_memory.h"
#include <assert.h>
int main(void) {
 const uint64_t gib=1ULL<<30, off=0x30000000;
 assert(output_heap_capacity(4*gib,0xcd500000)==4*gib);
 assert(output_heap_capacity(7*gib,0x1b3400000)==7*gib);
 uint64_t four=output_heap_capacity(4*gib,0x123900000);
 assert(output_low_size(6*gib,off,four)==6*gib);
 uint64_t five=output_heap_capacity(4*gib,0x177d00000);
 assert(five>=0x12fa00000ULL && five>=0x177d00000ULL);
 assert(!(five%0x200000));
 assert(output_low_size(6*gib,off,five)>=off+five);
 assert(output_heap_capacity(4*gib,0x177d00001)==0x177e00000ULL);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as t:
            exe=Path(t)/'output-memory'
            built=subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'games/totk/runtime'),'-x','c','-','-o',str(exe)],input=code,text=True,capture_output=True)
            self.assertEqual(built.returncode,0,built.stderr)
            ran=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(ran.returncode,0,ran.stderr)
