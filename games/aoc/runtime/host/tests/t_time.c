// Clock snapshot arithmetic (hle_time.c, from PR #36): CalculateSpanBetween and CalculateStandardUserSystemClockDifferenceByUser looked up by their mangled names in the real swforce
// registrations, run on snapshots made by the host's own CreateWithStandardSystemClock and on edited copies
#include "../hle_time.c"
#include <mach-o/getsect.h>

uint64_t guest_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
static int checks, fails;
extern const struct mach_header_64 _mh_execute_header;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; e && i < bytes / sizeof *e; i++) if (!strcmp(e[i].name, name)) { count++; fn = (void *)e[i].fn; }
    CHECK(count == 1 && fn, name);
    return count == 1 ? fn : NULL;
}
static void put64(uint8_t *s, int off, int64_t v) { memcpy(s + off, &v, 8); }
static int64_t get64(const uint8_t *s, int off) { int64_t v; memcpy(&v, s + off, 8); return v; }
int main(void) {
    Result (*snap)(uint8_t *) = hook("_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_");
    Result (*span)(int64_t *, const uint8_t *, const uint8_t *) = hook("_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_");
    int64_t (*udiff)(const uint8_t *, const uint8_t *) = hook("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_");
    if (!snap || !span || !udiff) { printf("time: %d checks, %d failed\n", checks, fails); return 1; }
    uint8_t a[0xd0], b[0xd0], c[0xd0]; int64_t ts = -1;
    CHECK(snap(a) == RES_OK, "snapshot a");
    CHECK(span(&ts, a, a) == RES_OK && ts == 0, "span of a snapshot with itself is 0");
    memcpy(b, a, sizeof b); put64(b, 0x90, get64(a, 0x90) + 90);   // b: 90 s of steady clock later
    CHECK(span(&ts, a, b) == RES_OK && ts == 90 * 1000000000LL, "span from the steady time points, in nanoseconds");
    CHECK(span(&ts, b, a) == RES_OK && ts == -90 * 1000000000LL, "span backwards is negative");
    memcpy(c, b, sizeof c); c[0x98] ^= 1; ts = 1234;   // c: another steady clock source, no network time
    CHECK(span(&ts, a, c) != RES_OK && ts == 1234, "different sources and no network time: an error, output untouched");
    put64(a, 0x48, 1000); put64(c, 0x48, 1600);   // with network times the span comes from them
    CHECK(span(&ts, a, c) == RES_OK && ts == 600 * 1000000000LL, "different sources: span from the network times");
    CHECK(snap(a) == RES_OK && snap(b) == RES_OK && udiff(a, b) == 0, "two host snapshots in a row: the user did not move the clock");
    memcpy(b, a, sizeof b); put64(b, 0x00, get64(a, 0x00) + 3600);   // b: the user clock offset one hour later
    CHECK(udiff(a, b) == 3600 * 1000000000LL, "user clock moved an hour");
    b[0x10] ^= 1; CHECK(udiff(a, b) == 0, "user contexts of different steady sources: 0");
    b[0x10] ^= 1; a[0xcc] = b[0xcc] = 1; CHECK(udiff(a, b) == 0, "automatic correction on in both: 0");
    a[0xcc] = 0; CHECK(udiff(a, b) == 3600 * 1000000000LL, "automatic correction in one only: the difference");
    printf("time: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
