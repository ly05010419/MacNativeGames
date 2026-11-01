"""Unavailable NGC data must produce an error, never a clean/masked result."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "ipc.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
static IpcHandler handler;
void runtime_ipc_register(const char *n,IpcHandler h,IpcOpen o,void (*c)(void *)){
 if(strcmp(n,"ngc:u")||o||c)exit(9);handler=h;
}
_Noreturn void ipc_unimplemented(const IpcSession *s,const IpcRequest *r){(void)s;(void)r;exit(4);}
static void *worker(void *unused){
 (void)unused;
 IpcSession s={.name="ngc:u"};
 for(unsigned loop=0;loop<1000;loop++)for(unsigned cmd=0;cmd<4;cmd++){
  unsigned char input[32],output[32],args[24];
  memset(input,0x5a,sizeof input);memset(output,0xa5,sizeof output);memset(args,0,sizeof args);
  IpcRequest r={.cmd=cmd,.data=args,.data_size=sizeof args,.na=1,.nb=1};
  r.a[0]=(IpcBuffer){(uintptr_t)input,sizeof input,0};
  r.b[0]=(IpcBuffer){(uintptr_t)output,sizeof output,0};
  IpcResponse out={0};handler(&s,&r,&out);
  if(out.result!=0xcc92||out.data_size||out.ncopy||out.nmove||out.nobjects||out.nxout)exit(9);
  for(unsigned i=0;i<32;i++)if(input[i]!=0x5a||output[i]!=0xa5)exit(9);
 }
 return NULL;
}
int main(int argc,char **argv){
 (void)argv;if(!handler)return 9;
 if(argc>1){IpcSession s={.name="ngc:u"};IpcRequest r={.cmd=4};IpcResponse out={0};handler(&s,&r,&out);return 9;}
 pthread_t threads[8];
 for(unsigned i=0;i<8;i++)if(pthread_create(threads+i,NULL,worker,NULL))return 9;
 for(unsigned i=0;i<8;i++)pthread_join(threads[i],NULL);
 return 0;
}
'''


class NgcHostTest(unittest.TestCase):
    def test_unavailable_is_error_without_outputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / 'probe.c', Path(tmp) / 'probe'
            source.write_text(HARNESS)
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'runtime'), str(source),
                            str(ROOT / 'runtime/ngc_host.c'), '-o', str(binary)], check=True)
            for args, expected in [([], 0), (['unknown'], 4)]:
                self.assertEqual(subprocess.run([str(binary), *args]).returncode, expected)
            # Prove the canary rejects the tempting blanket-success stub.
            mutation = Path(tmp) / 'mutated.c'
            production = (ROOT / 'runtime/ngc_host.c').read_text()
            self.assertEqual(production.count('rs->result = NGC_DATA_ACCESS_ERROR;'), 1)
            mutation.write_text(production.replace('rs->result = NGC_DATA_ACCESS_ERROR;',
                                                  'rs->result = 0;'))
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'runtime'), str(source), str(mutation),
                            '-o', str(binary)], check=True)
            self.assertEqual(subprocess.run([str(binary)]).returncode, 9)
