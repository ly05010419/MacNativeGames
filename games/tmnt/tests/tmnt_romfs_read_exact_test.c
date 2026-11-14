#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "../runtime/romfs_read_exact.h"

static const uint8_t source[9] = {3, 1, 4, 1, 5, 9, 2, 6, 5};
static int calls, interrupt_first, fail_after, eof_after;
static ssize_t test_pread(int fd, void *out, size_t n, off_t off) {
    (void)fd;
    calls++;
    if (interrupt_first && calls == 1) { errno = EINTR; return -1; }
    if (fail_after && calls >= fail_after) { errno = EIO; return -1; }
    if (eof_after && calls >= eof_after) return 0;
    assert(off >= 0 && (size_t)off < sizeof source);
    if (n > 2) n = 2;
    if (n > sizeof source - (size_t)off) n = sizeof source - (size_t)off;
    memcpy(out, source + off, n);
    return (ssize_t)n;
}
int main(void) {
    uint8_t out[9] = {0}; uint64_t done = 99;
    assert(!tmnt_romfs_read_exact(0,out,sizeof out,0,4,test_pread,&done));
    assert(done == sizeof out && !memcmp(out,source,sizeof out));
    memset(out,0,sizeof out);calls=0;interrupt_first=1;
    assert(!tmnt_romfs_read_exact(0,out,sizeof out,0,4,test_pread,&done));
    assert(done == sizeof out && !memcmp(out,source,sizeof out));
    interrupt_first=0;calls=0;fail_after=3;
    assert(tmnt_romfs_read_exact(0,out,sizeof out,0,4,test_pread,&done) < 0 && done == 4);
    fail_after=0;calls=0;eof_after=2;
    assert(tmnt_romfs_read_exact(0,out,sizeof out,0,4,test_pread,&done) < 0 && done == 2);
    eof_after=0;calls=0;
    assert(!tmnt_romfs_read_exact(0,out,0,0,4,test_pread,&done) && done == 0 && calls == 0);
    return 0;
}
