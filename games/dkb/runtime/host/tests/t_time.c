// Clock snapshot arithmetic (hle_time.c, from PR #36): CalculateSpanBetween and CalculateStandardUserSystemClockDifferenceByUser looked up by their mangled names in the real swforce
// registrations, run on both host snapshot entries and on edited copies
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
    Result (*adj_snap)(uint8_t *) = hook("_ZN2nn4time13ClockSnapshot31CreateWithAdjustableSystemClockEPS1_");
    unsigned (*span)(int64_t *, const uint8_t *, const uint8_t *) = hook("_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_");
    int64_t (*udiff)(const uint8_t *, const uint8_t *) = hook("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_");
    if (!snap || !adj_snap || !span || !udiff) { printf("time: %d checks, %d failed\n", checks, fails); return 1; }
    uint8_t a[0xd0], b[0xd0], c[0xd0]; int64_t ts = -1;
    // Before the first snapshot: h_time_GetCurrentTime reads DM_FIXED_TIME once and caches it
    setenv("DM_FIXED_TIME", "1700000000", 1);
    setenv("TZ", "UTC", 1); tzset();
    CHECK(snap(a) == RES_OK, "snapshot a");
    CHECK(a[0xcd] == 1, "standard snapshot type is 1");
    // Both entries must stay inside the SDK's 0xd0-byte buffer, including the type near its end.
    uint8_t guarded[0xf0]; memset(guarded, 0xa5, sizeof guarded);
    uint8_t *adjustable = guarded + 0x10;
    CHECK(snap(adjustable) == RES_OK, "standard snapshot in guarded buffer");
    int standard_guards_ok = 1;
    for (int i = 0; i < 0x10; i++) standard_guards_ok &= guarded[i] == 0xa5 && guarded[0xe0 + i] == 0xa5;
    CHECK(standard_guards_ok, "standard snapshot writes only its 0xd0 bytes");
    memset(guarded, 0xa5, sizeof guarded);
    CHECK(adj_snap(adjustable) == RES_OK, "adjustable snapshot");
    int guards_ok = 1;
    for (int i = 0; i < 0x10; i++) guards_ok &= guarded[i] == 0xa5 && guarded[0xe0 + i] == 0xa5;
    CHECK(guards_ok, "adjustable snapshot writes only its 0xd0 bytes");
    CHECK(adjustable[0xcd] == 2, "adjustable snapshot type is 2");
    CHECK(get64(adjustable, 0x40) == 1700000000, "adjustable user time follows the host clock");
    const CalendarTime *cal = (const CalendarTime *)(adjustable + 0x50);
    CHECK(cal->year == 2023 && cal->month == 11 && cal->day == 14 && cal->hour == 22 && cal->minute == 13 && cal->second == 20,
          "adjustable snapshot has the UTC calendar for the fixed time");
    uint8_t zero[0x20] = {0};
    CHECK(!memcmp(adjustable + 0x20, zero, sizeof zero) && get64(adjustable, 0x48) == 0,
          "offline snapshot has no network clock context or time");
    CHECK(!memcmp(a + 0x98, adjustable + 0x98, 16), "both snapshots use the same steady clock source");
    CHECK(udiff(a, adjustable) == 0, "changing snapshot type does not report a user clock change");
    CHECK(!strcmp((const char *)adjustable + 0xa8, "UTC"), "adjustable snapshot carries the host time zone");
    memcpy(b, a, sizeof b); b[0xcd] = 2;
    CHECK(span(&ts, a, b) == 1 && ts == 0, "changing only the type leaves the elapsed span at zero");
    unsetenv("DM_FIXED_TIME");
    CHECK(span(&ts, a, a) == 1 && ts == 0, "span of a snapshot with itself is 0");
    memcpy(b, a, sizeof b); put64(b, 0x90, get64(a, 0x90) + 90);   // b: 90 s of steady clock later
    CHECK(span(&ts, a, b) == 1 && ts == 90 * 1000000000LL, "span from the steady time points, in nanoseconds");
    CHECK(span(&ts, b, a) == 1 && ts == -90 * 1000000000LL, "span backwards is negative");
    memcpy(c, b, sizeof c); c[0x98] ^= 1; ts = 1234;   // c: another steady clock source, no network time
    CHECK(span(&ts, a, c) == 0 && ts == 1234, "different sources and no network time: false, output untouched");
    put64(a, 0x48, 1000); put64(c, 0x48, 1600);   // with network times the span comes from them
    CHECK(span(&ts, a, c) == 1 && ts == 600 * 1000000000LL, "different sources: span from the network times");
    CHECK(snap(a) == RES_OK && snap(b) == RES_OK && udiff(a, b) == 0, "two host snapshots in a row: the user did not move the clock");
    memcpy(b, a, sizeof b); put64(b, 0x00, get64(a, 0x00) + 3600);   // b: the user clock offset one hour later
    CHECK(udiff(a, b) == 3600 * 1000000000LL, "user clock moved an hour");
    b[0x10] ^= 1; CHECK(udiff(a, b) == 0, "user contexts of different steady sources: 0");
    b[0x10] ^= 1; a[0xcc] = b[0xcc] = 1; CHECK(udiff(a, b) == 0, "automatic correction on in both: 0");
    a[0xcc] = 0; CHECK(udiff(a, b) == 3600 * 1000000000LL, "automatic correction in one only: the difference");
    printf("time: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
