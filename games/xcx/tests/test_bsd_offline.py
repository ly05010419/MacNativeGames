"""Both SDK BSD endpoints share the existing offline-only request semantics."""
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
static IpcHandler user,system_handler;
void runtime_ipc_register(const char *n,IpcHandler h,IpcOpen o,void (*c)(void *)){
 (void)o;(void)c;if(!strcmp(n,"bsd:u"))user=h;if(!strcmp(n,"bsd:s"))system_handler=h;
}
uint64_t runtime_desired_language(void){return 0;}
_Noreturn void runtime_fail(const char *s){(void)s;exit(3);}
_Noreturn void ipc_unimplemented(const IpcSession *s,const IpcRequest *r){(void)s;(void)r;exit(4);}
void *ipc_guest(uint64_t a,uint64_t n){(void)n;return(void *)(uintptr_t)a;}
const void *ipc_arg(const IpcRequest *r,uint32_t o,uint32_t n){if(o+n>r->data_size)exit(3);return r->data+o;}
void ipc_out(IpcResponse *r,const void *p,uint32_t n){memcpy(r->data+r->data_size,p,n);r->data_size+=n;}
int main(int argc,char **argv){
 if(argc!=2||!user||user!=system_handler)return 9;
 IpcSession s={.name="bsd:s",.handler=system_handler};uint32_t args[]={2,1,0};
 IpcRequest r={.data=(const uint8_t *)args,.data_size=sizeof args};IpcResponse out={0};
 if(!strcmp(argv[1],"unknown")){r.cmd=99;system_handler(&s,&r,&out);return 9;}
 for(unsigned cmd=0;cmd<4;cmd++){
  r.cmd=cmd;memset(&out,0,sizeof out);system_handler(&s,&r,&out);
  if(out.result||out.ncopy||out.nmove||out.nobjects)return 9;
  if(cmd==1){if(out.data_size)return 9;}
  else if(cmd==0){uint64_t id=1;memcpy(&id,out.data,8);if(out.data_size!=8||id)return 9;}
  else {int32_t ret=0;uint32_t err=0;memcpy(&ret,out.data,4);memcpy(&err,out.data+4,4);if(out.data_size!=8||ret!=-1||err!=50)return 9;}
 }
 return 0;
}
'''


class OfflineBsdTest(unittest.TestCase):
    def test_both_endpoints_refuse_sockets(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / "probe.c", Path(tmp) / "probe"
            source.write_text(HARNESS)
            subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "runtime"), str(source),
                            str(ROOT / "runtime/system_services.c"), "-o", str(binary)], check=True)
            for mode, expected in [("valid", 0), ("unknown", 4)]:
                with self.subTest(mode=mode):
                    self.assertEqual(subprocess.run([str(binary), mode]).returncode, expected)
