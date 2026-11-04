"""Compile the real FSP control plane and reject malformed initialization."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "runtime.h"
#include "ipc.h"
#include <stdlib.h>
#include <string.h>
static IpcHandler handler;
static IpcOpen opener;
void runtime_ipc_register(const char *name,IpcHandler h,IpcOpen o,void (*close)(void *)){
 if(strcmp(name,"fsp-srv")||!close)exit(9);handler=h;opener=o;
}
_Noreturn void runtime_fail(const char *s){(void)s;exit(3);}
_Noreturn void ipc_unimplemented(const IpcSession *s,const IpcRequest *r){(void)s;(void)r;exit(4);}
void ipc_out(IpcResponse *r,const void *p,uint32_t n){memcpy(r->data+r->data_size,p,n);r->data_size+=n;}
int main(int argc,char **argv){
 if(argc!=2||!handler||!opener)return 9;
 IpcSession s={.name="fsp-srv",.handler=handler,.state=opener()};
 uint64_t placeholder=0;IpcRequest r={.cmd=1,.has_pid=1,.pid=123,.data=(void *)&placeholder,.data_size=8};
 IpcResponse out={0};
 if(!strcmp(argv[1],"before-init")){r.cmd=1005;handler(&s,&r,&out);return 9;}
 if(!strcmp(argv[1],"missing-pid"))r.has_pid=0;
 if(!strcmp(argv[1],"short-data"))r.data_size=7;
 handler(&s,&r,&out);
 if(out.result||out.data_size)return 9;
 r.cmd=!strcmp(argv[1],"unknown")?1006:1005;handler(&s,&r,&out);
 uint32_t mode=99;memcpy(&mode,out.data,4);free(s.state);
 return out.result||out.data_size!=4||mode!=0?9:0;
}
'''


class FsProxyTest(unittest.TestCase):
    def test_handshake_log_mode_and_rejections(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "probe.c"
            binary = Path(tmp) / "probe"
            source.write_text(HARNESS)
            subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "runtime"), str(source),
                            str(ROOT / "runtime/fs_proxy.c"), "-o", str(binary)], check=True)
            for mode, expected in [("valid", 0), ("before-init", 3), ("missing-pid", 3),
                                   ("short-data", 3), ("unknown", 4)]:
                with self.subTest(mode=mode):
                    self.assertEqual(subprocess.run([str(binary), mode]).returncode, expected)
