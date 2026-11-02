// NVN raw timestamp ABI: known vectors, long uptimes and an independent game's inline conversion.
#include "../nvn/nvn_timestamp.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, why) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", why); } } while (0)

// NieR:Automata 1.0.3 main build 3c10ec35c5ef4c3686b4053afb0e58e7, main+0x9ba7a8..0x9ba808.
// These are its integer instructions, including the two multiply-high divisions, before SCVTF / FDIV by 1000.
// The consumer never calls our conversion helper: 614400 raw ticks must become 1 ms in its own arithmetic.
static float nier_dr_ms(uint64_t ticks) {
    uint64_t a = ticks * 625;
    a = (uint64_t)((__uint128_t)a * UINT64_C(0xaaaaaaaaaaaaaaab) >> 64);
    uint64_t b = (a >> 8) & UINT32_MAX;
    a >>= 40;
    uint64_t c = a * UINT64_C(0xe353f7cf);
    uint64_t d = b * UINT64_C(0xe353f7cf);
    d = c + (d >> 32);
    a *= UINT64_C(0x20c49ba5);
    b *= UINT64_C(0x20c49ba5);
    b += (uint32_t)d;
    a += d >> 32;
    a += b >> 32;
    return (float)(int64_t)(a >> 7) / 1000.0f;
}

int main(void) {
    const struct { uint64_t ns, ticks; } exact[] = {
        {0, 0}, {625, 384}, {1000000, 614400}, {16000000, 9830400}, {1000000000, 614400000},
        {UINT64_C(5000000000000000000), UINT64_C(3072000000000000000)},
    };
    for (unsigned i = 0; i < sizeof exact / sizeof *exact; i++) {
        CHECK(nvn_timestamp_from_ns(exact[i].ns) == exact[i].ticks, "known ns -> 614.4 MHz ticks");
        CHECK(nvn_timestamp_to_ns(exact[i].ticks) == exact[i].ns, "known raw ticks -> ns");
    }
    CHECK(nvn_timestamp_from_ns(1) == 0 && nvn_timestamp_from_ns(2) == 1, "ns -> ticks rounds down");
    CHECK(nvn_timestamp_to_ns(1) == 1 && nvn_timestamp_to_ns(2) == 3, "ticks -> ns rounds down");
    const uint64_t ns[] = {1, 2, 624, 626, 16666667, UINT64_C(1234567890123456789), UINT64_MAX};
    for (unsigned i = 0; i < sizeof ns / sizeof *ns; i++) {
        uint64_t back = nvn_timestamp_to_ns(nvn_timestamp_from_ns(ns[i]));
        CHECK(back <= ns[i] && ns[i] - back <= 2, "ns round trip loses at most 2 ns, including long uptime");
    }
    CHECK(nier_dr_ms(614400) == 1.0f, "independent NieR raw counter conversion: 614400 ticks = 1 ms");
    CHECK(nier_dr_ms(nvn_timestamp_from_ns(16000000)) == 16.0f, "NieR sees a 16 ms GPU report as 16 ms");
    CHECK(nier_dr_ms(nvn_timestamp_from_ns(1000000000)) == 1000.0f, "NieR sees a 1 second GPU report as 1000 ms");
    CHECK(nier_dr_ms(307200) == 0.5f, "old 19.2 MHz report of 16 ms was read as 0.5 ms");
    printf("timestamp: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
