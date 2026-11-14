// nv::GetGraphicsMemoryInfo writes exactly the record its SDK defines: 0x20 bytes on SDK 10 and older, 0x40 from SDK 11 on (and when the version is unknown)
#include "../../gfx/hle_nv.c"
#include <assert.h>

static int sdk;
int dm_sdk_major(void) { return sdk; }

// The record between two guard words; after it, the old record again in an allocation of exactly its size, for AddressSanitizer
static void check(int major, uint64_t pool) {
    sdk = major;
    unsigned n = DM_SDK_OLD(major) ? 4 : 8;
    const uint64_t guard = UINT64_C(0xa5a5a5a5a5a5a5a5);
    struct { uint64_t before, out[8], after; } block;
    memset(&block, 0xa5, sizeof(block));
    h_nv_MemInfo(block.out);
    assert(block.before == guard && block.after == guard);
    for (unsigned i = 0; i < 8; i++) assert(block.out[i] == (i >= n ? guard : i == 0 || i == 2 ? pool : 0));
    uint64_t *exact = malloc(n * 8); assert(exact);
    h_nv_MemInfo(exact);
    free(exact);
}
int main(void) {
    static const int majors[] = {3, 10, 11, 20, 22, 0};
    unsigned char storage[4096];
    for (unsigned i = 0; i < sizeof(majors) / sizeof(majors[0]); i++) {
        check(majors[i], 0);
        h_nv_Init(storage, sizeof(storage));
        check(majors[i], sizeof(storage));
        h_nv_Fin();
        check(majors[i], 0);
    }
    puts("NV memory: the 0x20-byte record on SDK 10 and older, 0x40 bytes from SDK 11 on, pool figures in both");
    return 0;
}
