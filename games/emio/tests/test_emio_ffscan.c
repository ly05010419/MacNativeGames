// Self-test for the 0xFF-writer scanner (M1/G2): proves detection fires on a known pattern.
// Production scans run over queue/mutex word dumps (see emio_snap.c queue-words/mutex-words).
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
// Detector under test: returns 1 if any 0xFF byte run of length>=4 exists in the blob.
static int ff_run(const unsigned char *b, size_t n) {
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        if (b[i] == 0xFF) { if (++run >= 4) return 1; }
        else run = 0;
    }
    return 0;
}
int main(void) {
    unsigned char clean[64]; memset(clean, 0, sizeof clean);
    // pointer-like content (mimics queue words: addresses, counts, flags - has isolated 0xFF bytes but no runs)
    uint64_t words[] = { 0x70b8fb6230ULL, 0x100000000ULL, 0x1001ULL, 0x70b8fb626cULL };
    memcpy(clean, words, sizeof words);
    assert(!ff_run(clean, sizeof clean));
    unsigned char dirty[64]; memcpy(dirty, clean, sizeof clean);
    memset(dirty + 16, 0xFF, 8); // simulated flash-erase writer hit
    assert(ff_run(dirty, sizeof dirty));
    // single/double 0xFF (address bytes) must not fire
    unsigned char addrff[8] = { 0x30, 0x62, 0xFB, 0xB8, 0x70, 0x00, 0x00, 0xFF };
    assert(!ff_run(addrff, sizeof addrff));
    printf("ffscan self-test passed (clean clean, dirty fires, addr-bytes silent)\n");
    return 0;
}
