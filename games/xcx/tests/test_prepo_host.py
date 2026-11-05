"""The SDK's session-id query must work without transmitting play reports."""
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
 if(strcmp(n,"prepo:u")||o||c)exit(9);handler=h;
}
_Noreturn void runtime_fail(const char *s){(void)s;exit(3);}
_Noreturn void ipc_unimplemented(const IpcSession *s,const IpcRequest *r){(void)s;(void)r;exit(4);}
void ipc_out(IpcResponse *r,const void *p,uint32_t n){
 if(n>sizeof r->data-r->data_size)exit(9);
 memcpy(r->data+r->data_size,p,n);r->data_size+=n;
}
static uint64_t query(void){
 IpcSession s={.name="prepo:u",.handler=handler};
 IpcRequest r={.cmd=10400};IpcResponse out={0};handler(&s,&r,&out);
 uint64_t id;memcpy(&id,out.data,8);
 if(out.result||out.data_size!=8||out.ncopy||out.nmove||out.nobjects||out.nxout||!id)exit(9);
 return id;
}
static void *worker(void *p){(void)p;uint64_t *id=malloc(sizeof *id);if(!id)exit(9);
 *id=query();for(unsigned i=0;i<10000;i++)if(query()!=*id)exit(9);return id;
}
int main(int argc,char **argv){
 (void)argv;if(!handler)return 9;
 if(argc>1){IpcSession s={.name="prepo:u"};IpcRequest r={.cmd=10104};IpcResponse out={0};handler(&s,&r,&out);return 9;}
 pthread_t threads[8];uint64_t id=0;
 for(unsigned i=0;i<8;i++)if(pthread_create(threads+i,NULL,worker,NULL))return 9;
 for(unsigned i=0;i<8;i++){void *p;pthread_join(threads[i],&p);uint64_t v=*(uint64_t *)p;free(p);if(id&&v!=id)return 9;id=v;}
 return 0;
}
'''


class PrepoHostTest(unittest.TestCase):
    def test_session_query_and_unknown_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / 'probe.c', Path(tmp) / 'probe'
            source.write_text(HARNESS)
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'runtime'), str(source),
                            str(ROOT / 'runtime/prepo_host.c'), '-o', str(binary)], check=True)
            for args, expected in [([], 0), (['unknown'], 4)]:
                self.assertEqual(subprocess.run([str(binary), *args]).returncode, expected)
