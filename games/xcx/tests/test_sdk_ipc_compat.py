"""The opt-in SDK adapter must validate every site before altering any text."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
static unsigned char text[0xa0000], snapshot[sizeof text];
static int enabled=1;
int runtime_extended_va(void){return enabled;}
uint8_t *runtime_module_base(const char *name){(void)name;return NULL;}
void runtime_thread_init(void){}
_Noreturn void runtime_fail(const char *message){(void)message;exit(memcmp(text,snapshot,sizeof text)?8:7);}
int main(int argc,char **argv){
 if(argc!=2)return 9;
 const uint32_t sites[][2]={{0x9a188,0x121e0b5a},{0x9a240,0x121e0b7b},{0x9a2cc,0x121e0b5a},
  {0x9a3b8,0x121e0b5a},{0x9a42c,0x121a0b18},{0x9a670,0x121a0a31},{0x9a784,0x121e0a31}};
 for(unsigned i=0;i<7;i++)memcpy(text+sites[i][0],&sites[i][1],4);
 Module m={.name="sdk",.digest="9d629869f383b2966067391a3c3cf2102550274f523c0d550fff7cde1b605680",.base=text,.text_end=sizeof text};
 if(!strcmp(argv[1],"last-site"))text[sites[6][0]]^=1;
 if(!strcmp(argv[1],"identity"))m.digest="other image";
 if(!strcmp(argv[1],"bounds"))m.text_end=sites[6][0];
 if(!strcmp(argv[1],"disabled"))enabled=0;
 if(!strcmp(argv[1],"other-module"))m.name="main";
 memcpy(snapshot,text,sizeof text);
 runtime_sdk_ipc_compat(&m);
 if(!strcmp(argv[1],"valid"))for(unsigned i=0;i<7;i++){uint32_t patched=sites[i][1]+0x400;memcpy(snapshot+sites[i][0],&patched,4);}
 return memcmp(text,snapshot,sizeof text)?9:0;
}
'''


class SdkIpcCompatTest(unittest.TestCase):
    def test_opt_in_and_atomic_identity_checks(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / "probe.c", Path(tmp) / "probe"
            source.write_text(HARNESS)
            subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "runtime"), str(source),
                            str(ROOT / "runtime/sdk_ipc_compat.c"), "-o", str(binary)], check=True)
            for mode, expected in [("valid", 0), ("last-site", 7), ("identity", 7),
                                   ("bounds", 7), ("disabled", 0), ("other-module", 0)]:
                with self.subTest(mode=mode):
                    self.assertEqual(subprocess.run([str(binary), mode]).returncode, expected)
