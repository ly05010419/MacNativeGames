"""The guest libc++ clock returns microseconds, not Darwin chrono ticks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "runtime.h"
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
_Noreturn void runtime_fail(const char *s){(void)s;exit(3);}
extern void *runtime_time_override(const char *);
static int64_t realtime_us(void){struct timespec t; if(clock_gettime(CLOCK_REALTIME,&t))exit(9);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
int main(void){
 int64_t (*now)(void)=runtime_time_override("_ZNSt3__16chrono12system_clock3nowEv");
 if(!now||runtime_time_override("unknown"))return 9;
 int64_t before=realtime_us(),value=now(),after=realtime_us();
 return value<before||value>after?9:0;
}
'''


class TimeHostTest(unittest.TestCase):
    def test_guest_epoch_and_microsecond_abi(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'probe.c'
            binary = Path(tmp) / 'probe'
            source.write_text(HARNESS)
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'runtime'), str(source),
                            str(ROOT / 'runtime/time_host.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
