"""The actual iOS close function waits for save completion and accepts repeated close requests once."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'requires macOS dispatch')
class IosCloseTests(unittest.TestCase):
    def test_close_waits_for_save_then_exits_once(self):
        source = (ROOT / 'games/oot/runtime/frontend_ios.m').read_text()
        start = source.index('void runtime_front_request_close(void){')
        end = source.index('\nvoid runtime_front_text_test', start)
        function = source[start:end]
        probe = r'''
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#include <unistd.h>
static _Atomic int busy=1, exits=0, flushed=0;
static dispatch_semaphore_t ended;
static int runtime_fs_save_busy(void) { return atomic_load(&busy); }
static void runtime_cloud_save_quit(void) { atomic_fetch_add(&flushed,1); }
static void test_exit(int code) { if(code) abort(); atomic_fetch_add(&exits,1); dispatch_semaphore_signal(ended); }
#define _exit test_exit
''' + function + r'''
int main(void) {
 ended=dispatch_semaphore_create(0);
 runtime_front_request_close();runtime_front_request_close();
 if(dispatch_semaphore_wait(ended,dispatch_time(DISPATCH_TIME_NOW,100000000))==0 || exits || flushed) return 1;
 atomic_store(&busy,0);
 if(dispatch_semaphore_wait(ended,dispatch_time(DISPATCH_TIME_NOW,2000000000))) return 2;
 runtime_front_request_close();usleep(50000);
 return exits==1 && flushed==1 ? 0 : 3;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / 'probe.m').write_text(probe)
            result = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Werror', str(path / 'probe.m'),
                                     '-framework', 'Foundation', '-o', str(path / 'probe')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([path / 'probe'], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
