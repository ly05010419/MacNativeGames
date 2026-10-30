"""Local UUID writes exactly 16 bytes and survives a process restart."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Noreturn void runtime_fail(const char *message){(void)message;exit(7);}
int main(void){
 unsigned char guarded[32],again[16];memset(guarded,0xa5,sizeof guarded);
 runtime_pseudo_device_id(guarded+8);runtime_pseudo_device_id(again);
 if(memcmp(guarded+8,again,16)||(again[6]>>4)!=8||(again[8]>>6)!=2)return 9;
 for(unsigned i=0;i<8;i++)if(guarded[i]!=0xa5||guarded[i+24]!=0xa5)return 9;
 return fwrite(again,1,16,stdout)==16?0:9;
}
'''


class PseudoDeviceTest(unittest.TestCase):
    def test_stable_profile_identity_and_write_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / "probe.c", Path(tmp) / "probe"
            source.write_text(HARNESS)
            subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-deprecated-declarations", "-I", str(ROOT / "runtime"), str(source),
                            str(ROOT / "runtime/pseudo_device.c"), str(ROOT / "runtime/account.c"),
                            "-o", str(binary)], check=True)
            def identity(profile):
                return subprocess.run([str(binary)], env=os.environ | {"TOTK_PROFILE_DIR": str(profile)},
                                      check=True, capture_output=True).stdout
            first = identity(Path(tmp) / "first")
            self.assertEqual(len(first), 16)
            self.assertEqual(first, identity(Path(tmp) / "first"))
            self.assertNotEqual(first, identity(Path(tmp) / "other"))
