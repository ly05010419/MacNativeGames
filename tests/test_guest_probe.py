"""The texture upload's readability probe reads paged-out guest memory on several threads (2026-11-19, user: "两步都做
先并行 再提前").

Ocarina of Time on the user's route: the first upload of a texture read pages the system had paged out (mincore: 127 of
128 pages of a 2 MB texture), 70-200 us a page and 9-25 ms a texture on the submit thread. nvn_guest_probe.h reads a
range of 32 pages or more in chunks on several threads, each with its own thread-local fault guard. The answer must be
the one the single-thread probe gives: every page is read (a single unreadable page anywhere is found), ranges that do
not start or end on a page are covered, and TOTK_PROBE_PARALLEL=0 is the old single-thread read."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'

PROBE = r'''
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <setjmp.h>
__thread sigjmp_buf *runtime_fault_guard;
#include "nvn_guest_probe.h"
static void on_fault(int num, siginfo_t *info, void *ctx) {
    (void)info; (void)ctx;
    if (runtime_fault_guard) { sigjmp_buf *g = runtime_fault_guard; runtime_fault_guard = NULL; siglongjmp(*g, 1); }
    signal(num, SIG_DFL);
}
int main(void) {
    struct sigaction sa = {0}; sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    size_t pg = (size_t)getpagesize(), n = 100;   /* 13 chunks of 8 pages, the last one short */
    uint8_t *m = mmap(NULL, n * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(m != MAP_FAILED);
    for (size_t i = 0; i < n; i++) m[i * pg] = (uint8_t)i;
    int checks = 0;
    assert(nvn_guest_probe(m, n * pg) == 1 && nvn_probe_parallel_on()); checks++;   /* on unless TOTK_PROBE_PARALLEL=0 */
    for (int par = 0; par < 2; par++) {
        assert(nvn_guest_probe_threads(m, n * pg, par) == 1); checks++;
        assert(nvn_guest_probe_threads(m + 7, n * pg - 9, par) == 1); checks++;    /* not on page bounds */
        assert(nvn_guest_probe_threads(m, 0, par) == 1); checks++;
        for (size_t i = 0; i < n; i++) {   /* each page alone unreadable: found wherever it is */
            assert(!mprotect(m + i * pg, pg, PROT_NONE));
            assert(nvn_guest_probe_threads(m, n * pg, par) == 0); checks++;
            assert(nvn_guest_probe_threads(m + 7, n * pg - 9, par) == 0); checks++;
            assert(!mprotect(m + i * pg, pg, PROT_READ | PROT_WRITE));
        }
        assert(!mprotect(m + (n - 1) * pg, pg, PROT_NONE));   /* the last byte's page, from a range ending inside it */
        assert(nvn_guest_probe_threads(m + pg, (n - 2) * pg + 1, par) == 0); checks++;
        assert(nvn_guest_probe_threads(m + pg, (n - 2) * pg, par) == 1); checks++;   /* ...and one ending before it */
        assert(!mprotect(m + (n - 1) * pg, pg, PROT_READ | PROT_WRITE));
    }
    assert(runtime_fault_guard == NULL);
    printf("guest probe: ok %d\n", checks);
    return 0;
}
'''


class GuestProbe(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'darwin', 'clang and dispatch')
    def test_every_page_is_read_on_one_thread_and_on_several(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'probe.c'
            src.write_text(PROBE)
            exe = Path(tmp) / 'probe'
            subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-fblocks', '-I', str(NVN),
                            str(src), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
            self.assertEqual(out.strip(), 'guest probe: ok 411')

    def test_the_upload_uses_it(self):
        mtl = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('#include "nvn_guest_probe.h"', mtl)
        self.assertIn('static int guest_probe(const void *mem, uint64_t size) { return nvn_guest_probe(mem, size); }', mtl)
        header = (NVN / 'nvn_guest_probe.h').read_text()
        self.assertIn('getenv("TOTK_PROBE_PARALLEL")', header)   # =0: the old single-thread read


if __name__ == '__main__':
    unittest.main()
