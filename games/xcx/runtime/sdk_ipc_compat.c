/* XCX 1.0.0's actual SDK descriptor encoder, not a synthetic HIPC roundtrip. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* The pinned SDK narrows X/A/B/W descriptor addresses to 39 bits even though
   HIPC has room for more. Our opt-in Mac layout needs bit 39. Extend only the
   seven address masks in this verified encoder, retaining all other fields.
   Input image hashes and rewrite metadata remain unchanged. Validate every
   site before writing anything; text is still RW and is sealed RX afterward. */
void runtime_sdk_ipc_compat(Module *m) {
    if (strcmp(m->name, "sdk") || !runtime_extended_va()) return;
    static const struct { uint32_t offset, before; } sites[] = {
        {0x9a188,0x121e0b5a},{0x9a240,0x121e0b7b},
        {0x9a2cc,0x121e0b5a},{0x9a3b8,0x121e0b5a},
        {0x9a42c,0x121a0b18},{0x9a670,0x121a0a31},
        {0x9a784,0x121e0a31}
    };
    if (strcmp(m->digest,"9d629869f383b2966067391a3c3cf2102550274f523c0d550fff7cde1b605680")) runtime_fail("SDK IPC compatibility image identity");
    for (unsigned i=0;i<sizeof sites/sizeof *sites;i++) {
        uint32_t word;
        if (sites[i].offset+4>m->text_end) runtime_fail("SDK IPC compatibility text bounds");
        memcpy(&word,m->base+sites[i].offset,4);
        if (word!=sites[i].before) runtime_fail("SDK IPC compatibility instruction mismatch");
    }
    for (unsigned i=0;i<sizeof sites/sizeof *sites;i++) {
        /* AArch64 logical immediate: one more consecutive set bit, same shift.
           #0x1c -> #0x3c, #0x1c0 -> #0x3c0. No unrelated bits change. */
        uint32_t word=sites[i].before+0x400;
        memcpy(m->base+sites[i].offset,&word,4);
    }
    fprintf(stderr,"[xcx] SDK IPC: verified 7 address masks extended to 40 bits\n");
}

void runtime_sdk_ipc_probe(void) {
    uint8_t *sdk = runtime_module_base("sdk");
    if (!sdk) runtime_fail("SDK IPC probe module");
    runtime_thread_init();
    typedef uint32_t (*Encode)(void *, int, const uint64_t *, const uint32_t *, uint64_t, uint64_t);
    Encode encode = (Encode)(void *)(sdk + 0x9a0f0);
    const struct { uint32_t attr; uint64_t size, capacity; unsigned word; int mapped; } cases[] = {
        {5, 0x100, 0, 16, 1}, {6, 0x100, 0, 24, 1},
        {0x21, 0x10000, 0, 16, 1}, {0x22, 0x10000, 0, 24, 1},
        {9, 0x100, 0x1000, 8, 0}, {0x21, 0x100, 0x1000, 8, 0},
        {0x22, 0x100, 0, 24, 1}
    };
    const uint64_t addresses[] = {0x123456780ULL, 0x7fffffffffULL, 0x8000000000ULL, 0x81c0102340ULL, 0xffffffffffULL};
    unsigned checked = 0;
    for (unsigned a = 0; a < sizeof addresses / sizeof *addresses; a++) {
        for (unsigned k = 0; k < sizeof cases / sizeof *cases; k++) {
            uint8_t layout[32] = {0};
            layout[0x13]=8;layout[0x14]=16;layout[0x15]=24;layout[0x17]=40;layout[0x18]=48;
            struct { uint32_t before[4], msg[64], after[4]; } out;
            memset(&out, 0xa5, sizeof out);memset(out.msg, 0, sizeof out.msg);
            void *context[] = {layout, out.msg};
            uint64_t buffer[] = {addresses[a], cases[k].size};
            uint32_t attr = cases[k].attr;
            if (encode(context, 1, buffer, &attr, cases[k].capacity, 0)) runtime_fail("SDK IPC encoder result");
            const uint32_t *d = out.msg + cases[k].word;
            uint64_t got = cases[k].mapped ? d[1] | ((uint64_t)(d[2] >> 28) << 32) | ((uint64_t)((d[2] >> 2) & 0x3fffff) << 36)
                : d[1] | ((uint64_t)((d[0] >> 12) & 15) << 32) | ((uint64_t)((d[0] >> 6) & 63) << 36);
            if (got != addresses[a]) {
                fprintf(stderr, "[xcx-sdk-ipc] case=%u attr=%#x address=%llx encoded=%llx\n", k, attr, (unsigned long long)addresses[a], (unsigned long long)got);
                runtime_fail("original SDK IPC address roundtrip");
            }
            uint64_t size=cases[k].mapped?d[0]|((uint64_t)((d[2]>>24)&15)<<32):d[0]>>16;
            if(size!=cases[k].size||(cases[k].mapped&&(d[2]&3)))runtime_fail("SDK IPC size or mode changed");
            for (unsigned i=0;i<4;i++) if(out.before[i]!=0xa5a5a5a5||out.after[i]!=0xa5a5a5a5)runtime_fail("SDK IPC encoder write boundary");
            checked++;
        }
    }
    printf("{\"status\":\"pass\",\"scope\":\"original SDK mapped/static/automatic IPC address encoding, bit 39 and write canaries\",\"checks\":%u}\n",checked);
}
