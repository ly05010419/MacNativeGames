"""Exit protection must balance nesting and keep close requests deferred."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "runtime.h"
#include <stdlib.h>
#include <pthread.h>
uint64_t runtime_now_ns(void){return 0;}
int runtime_core_away(void){return 0;}
void runtime_core_back(int *away){(void)away;}
void hle_bind_system_event(void *event,int autoclear){(void)event;(void)autoclear;}
void runtime_pseudo_device_id(uint8_t *out){(void)out;}
static unsigned drain_exit(void){
 unsigned char (*pop)(uint32_t *)=runtime_applet_override("_ZN2nn2oe25TryPopNotificationMessageEPj");
 uint32_t message;unsigned exits=0;
 while(pop(&message)){if(message==4)exits++;else if(message!=15)exit(9);}
 return exits;
}
static void *close_worker(void *p){(void)p;return runtime_exit_request_close()?(void *)1:NULL;}
_Noreturn void runtime_fail(const char *s){(void)s;exit(3);}
static void *worker(void *p){(void)p;for(int i=0;i<10000;i++){runtime_exit_section_enter();runtime_exit_section_leave();}return NULL;}
int main(int argc,char **argv){
 (void)argv;
 if(runtime_exit_section_active())return 9;
 if(argc>1){runtime_exit_section_leave();return 9;}
 if(!runtime_exit_request_close()||drain_exit())return 9;
 runtime_exit_section_enter();runtime_exit_section_enter();runtime_exit_section_leave();
 if(!runtime_exit_section_active())return 9;
 pthread_t t[4];for(int i=0;i<4;i++)if(pthread_create(t+i,NULL,worker,NULL))return 9;
 for(int i=0;i<4;i++)pthread_join(t[i],NULL);
 if(!runtime_exit_section_active())return 9;
 /* A storm of focus changes must leave room for the critical exit request. */
 for(int i=0;i<40;i++)runtime_oe_push_message(15);
 for(int i=0;i<4;i++)if(pthread_create(t+i,NULL,close_worker,NULL))return 9;
 for(int i=0;i<4;i++){void *r;pthread_join(t[i],&r);if(r)return 9;}
 if(drain_exit()!=1)return 8;
 if(runtime_exit_request_close()||drain_exit())return 9;
 runtime_exit_section_leave();
 return runtime_exit_section_active()||!runtime_exit_request_close()||drain_exit()?9:0;
}
'''


class ExitHostTest(unittest.TestCase):
    def test_nested_and_concurrent_guards_and_unbalanced_leave(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'probe.c'
            binary = Path(tmp) / 'probe'
            source.write_text(HARNESS)
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'runtime'), str(source),
                            str(ROOT / 'runtime/exit_host.c'), str(ROOT / 'runtime/applet_host.c'), '-o', str(binary)], check=True)
            self.assertEqual(subprocess.run([str(binary)]).returncode, 0)
            self.assertEqual(subprocess.run([str(binary), 'unbalanced']).returncode, 3)
