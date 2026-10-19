// CPU-only regression of the production serial-history representation.
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/totk_serial_set.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    totk_serial_set s = {0};
    const uint64_t edges[] = {0, 1, 63, 64, 65535, 65536, 65537, (UINT64_C(1)<<32)-1,
        UINT64_C(1)<<32, UINT64_C(1)<<63, UINT64_MAX-1, UINT64_MAX};
    for (unsigned i = 0; i < sizeof edges / sizeof *edges; i++) {
        assert(!totk_serial_has(&s, edges[i])); // caches the absent page
        assert(totk_serial_add(&s, edges[i])); // must invalidate that cached miss
        assert(!totk_serial_add(&s, edges[i]));
    }
    for (unsigned i = 0; i < sizeof edges / sizeof *edges; i++) assert(totk_serial_has(&s, edges[i]));
    assert(!totk_serial_has(&s, 62)); assert(!totk_serial_has(&s, UINT64_MAX-2));
    // Hit the old large-table growth regime, then revisit every older fact.
    // Two million values fit in 31 leaf pages instead of millions of hash slots.
    const uint64_t n = 2000000, base = UINT64_C(1)<<40;
    for (uint64_t i = 0; i < n; i++) assert(totk_serial_add(&s, base+i));
    for (uint64_t i = 0; i < n; i++) assert(totk_serial_has(&s, base+i));
    for (unsigned i = 0; i < sizeof edges / sizeof *edges; i++) assert(totk_serial_has(&s, edges[i]));
    assert(!totk_serial_has(&s, base+n));
    assert(s.n == n+sizeof edges/sizeof *edges);
    assert(s.bytes < 512*1024); // Includes the radix index and sparse high serials.
    printf("PASS: exact 64-bit history, cached-miss invalidation, old facts retained; %llu entries, %llu bytes\n",
        (unsigned long long)s.n, (unsigned long long)s.bytes);
    totk_serial_destroy(&s); assert(!s.n && !s.bytes && !s.root);
    assert(!totk_serial_has(&s, 0));
    return 0;
}
