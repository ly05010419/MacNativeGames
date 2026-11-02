/* nn::time for the functions main imports, backed by the host clock and time zone.
   User and network system clocks both follow the (NTP-synchronised) host wall clock; the steady clock
   counts seconds of host continuous time with one source id per process. ClockSnapshot uses the public
   0xD0-byte layout because game code may read its fields through inline accessors. */
#include "runtime.h"
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { int64_t value; uint8_t source[16]; } SteadyClockTimePoint;
typedef struct { int64_t offset; SteadyClockTimePoint point; } SystemClockContext;
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;
typedef struct { int32_t day_of_week, day_of_year; char name[8]; uint8_t dst, pad[3]; int32_t utc_offset; } CalendarAdditionalInfo;
typedef struct {
    SystemClockContext user, network; int64_t user_time, network_time;
    CalendarTime user_calendar, network_calendar; CalendarAdditionalInfo user_info, network_info;
    SteadyClockTimePoint steady; char location[0x24]; uint8_t automatic_correction, type, pad[2];
} ClockSnapshot;
_Static_assert(sizeof(SteadyClockTimePoint) == 0x18 && sizeof(CalendarTime) == 8 && sizeof(CalendarAdditionalInfo) == 0x18, "nn::time types");
_Static_assert(sizeof(ClockSnapshot) == 0xd0, "nn::time::ClockSnapshot");

#define R_INVALID 0x74u          /* time module 116: invalid argument family */
#define R_SOURCE_MISMATCH 0x18074u
static uint8_t source_id[16];
static int initialized;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void make_source(void) { arc4random_buf(source_id, sizeof source_id); }

static int64_t steady_seconds(void) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    return (int64_t)((__uint128_t)mach_continuous_time() * tb.numer / tb.denom / 1000000000u);
}
static SteadyClockTimePoint steady_now(void) { pthread_once(&once, make_source); SteadyClockTimePoint p = {steady_seconds(), {0}}; memcpy(p.source, source_id, 16); return p; }
static int64_t posix_now(void) {
    static long off = -1; if (off < 0) { const char *e = getenv("TOTK_TIME_OFFSET"); off = e ? atol(e) : 0; if (off < 0) off = 0; }   /* diagnostic: shift the clock the game sees (seconds) */
    return (int64_t)time(NULL) + off;
}

static unsigned initialize(void) { initialized = 1; pthread_once(&once, make_source); return 0; }
static unsigned finalize(void) { initialized = 0; return 0; }
static unsigned char is_initialized(void) { return (unsigned char)initialized; }
static unsigned user_time(int64_t *out) { if (!out) return R_INVALID; *out = posix_now(); return 0; }
/* StandardSteadyClock::now() returns the chrono-style time_point: the int64 seconds value only, 0 without a source
   (sdk+0x3d1d88 returns it in x0; returning the 0x18-byte struct made the host write through the caller's stale x8). */
static int64_t steady_clock_now(void) { return steady_now().value; }
/* SDK 7 (Breath of the Wild 1.6.0, botw.md): the steady clock is read into a time point, and a span is only defined between
   points of the same clock source. */
static uint32_t steady_time_point(SteadyClockTimePoint *out) { if (!out) runtime_fail("steady clock time point"); *out = steady_now(); return 0; }
static uint32_t span_between_points(int64_t *out, const SteadyClockTimePoint *a, const SteadyClockTimePoint *b) {
    if (!out || !a || !b) runtime_fail("time span between points");
    if (memcmp(a->source, b->source, 16)) return 0x2a6u;          /* time: not comparable (different clock sources) */
    *out = b->value - a->value; return 0;
}

static void fill_calendar(CalendarTime *c, CalendarAdditionalInfo *info, int64_t posix, int local) {
    time_t t = (time_t)posix; struct tm tm;
    if (!(local ? localtime_r(&t, &tm) : gmtime_r(&t, &tm))) runtime_fail("calendar conversion");
    *c = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (info) {
        memset(info, 0, sizeof *info);
        info->day_of_week = tm.tm_wday; info->day_of_year = tm.tm_yday;
        snprintf(info->name, sizeof info->name, "%s", local && tm.tm_zone ? tm.tm_zone : "UTC");
        info->dst = local && tm.tm_isdst > 0; info->utc_offset = local ? (int32_t)tm.tm_gmtoff : 0;
    }
}
static unsigned to_calendar(CalendarTime *c, CalendarAdditionalInfo *info, const int64_t *posix) {
    if (!c || !posix) return R_INVALID; fill_calendar(c, info, *posix, 1); return 0;
}
static CalendarTime to_calendar_utc(const int64_t *posix) { CalendarTime c; fill_calendar(&c, NULL, posix ? *posix : 0, 0); return c; }
static int64_t to_posix_utc(const CalendarTime *c) {
    struct tm tm = {0}; tm.tm_year = c->year - 1900; tm.tm_mon = c->month - 1; tm.tm_mday = c->day; tm.tm_hour = c->hour; tm.tm_min = c->minute; tm.tm_sec = c->second;
    return (int64_t)timegm(&tm);
}

static void location_name(char out[0x24]) {
    memset(out, 0, 0x24); char link[512]; ssize_t n = readlink("/etc/localtime", link, sizeof link - 1);
    if (n > 0) { link[n] = 0; const char *z = strstr(link, "zoneinfo/"); snprintf(out, 0x24, "%s", z ? z + 9 : "UTC"); }
    else snprintf(out, 0x24, "UTC");
}
static void snapshot_init(ClockSnapshot *s) { memset(s, 0, sizeof *s); }
static unsigned snapshot_create(ClockSnapshot *s) {
    if (!s) return R_INVALID;
    SteadyClockTimePoint steady = steady_now(); int64_t now = posix_now();
    memset(s, 0, sizeof *s);
    s->user = (SystemClockContext){now - steady.value, steady}; s->network = s->user;
    s->user_time = now; s->network_time = now;
    fill_calendar(&s->user_calendar, &s->user_info, now, 1); fill_calendar(&s->network_calendar, &s->network_info, now, 1);
    s->steady = steady; location_name(s->location); s->automatic_correction = 1; s->type = 0;
    return 0;
}
/* Snapshots of different steady clock sources (this host draws a new source each run, so every snapshot a save holds from
   an earlier run) are measured by their network clock times, as the time service does (Ryujinx IStaticServiceForPsc and
   yuzu psc/time/static.cpp CalculateSpanBetween); only without both network times is the span not found.
   The SDK's nn::time::CalculateSpanBetween returns a bool, not an nn::Result (sdk+0x38f030: 1 after storing the span, 0
   when the service failed), so span_between answers 1 / 0. It answered 0 on success and a nonzero result on failure, the
   game read every computed span as failed, and Super Smash Bros. Ultimate's Spirit Board replayed its refill animation
   without end (2026-10-24, Q66; about 970 comparisons a second, only on the board). */
static unsigned span_between_raw(int64_t *out_ns, const ClockSnapshot *a, const ClockSnapshot *b, int *how) {
    if (!out_ns || !a || !b) return R_INVALID;
    if (!memcmp(a->steady.source, b->steady.source, 16)) { *how = 0; *out_ns = (b->steady.value - a->steady.value) * 1000000000LL; return 0; }
    if (!a->network_time || !b->network_time) { *how = 2; return R_SOURCE_MISMATCH; }
    *how = 1; *out_ns = (b->network_time - a->network_time) * 1000000000LL; return 0;
}
/* Q66 diagnostic: what the game compares. The first 24 calls in full (steady values, same source or not, network times,
   result, span), then a summary every 20,000 calls: same source / network fallback / not found, empty snapshots (never
   filled: zero source and zero network time) on either side, and the smallest and largest span. Logging only. */
static int snapshot_empty(const ClockSnapshot *s) { static const uint8_t z[16]; return !s->network_time && !memcmp(s->steady.source, z, 16); }
static uint8_t span_between(int64_t *out_ns, const ClockSnapshot *a, const ClockSnapshot *b) {
    int how = -1; unsigned r = span_between_raw(out_ns, a, b, &how);
    static _Atomic uint64_t calls, kind[3], empty_a, empty_b; static _Atomic int64_t lo = INT64_MAX, hi = INT64_MIN;
    uint64_t n = ++calls;
    if (how >= 0 && how < 3) kind[how]++;
    if (a && snapshot_empty(a)) empty_a++;
    if (b && snapshot_empty(b)) empty_b++;
    if (!r) { int64_t s = *out_ns / 1000000000LL, v;
        while (s < (v = lo) && !atomic_compare_exchange_weak(&lo, &v, s)) {}
        while (s > (v = hi) && !atomic_compare_exchange_weak(&hi, &v, s)) {} }
    if (n <= 24 && a && b)
        fprintf(stderr, "[totk-time] span #%llu: steady %lld -> %lld (%s source), network %lld -> %lld, result %#x, span %lld s\n", (unsigned long long)n,
                (long long)a->steady.value, (long long)b->steady.value, how == 0 ? "same" : "other", (long long)a->network_time, (long long)b->network_time,
                r, r ? 0LL : (long long)(*out_ns / 1000000000LL));
    if (n % 20000 == 0)
        fprintf(stderr, "[totk-time] %llu spans: same source %llu, by network time %llu, not found %llu; empty snapshot first %llu second %llu; span %lld..%lld s\n",
                (unsigned long long)n, (unsigned long long)kind[0], (unsigned long long)kind[1], (unsigned long long)kind[2], (unsigned long long)empty_a,
                (unsigned long long)empty_b, (long long)lo, (long long)hi);
    return r == 0;   // the SDK's bool: span stored
}
/* Change of the user clock offset between snapshots, i.e. manual adjustments, as a TimeSpan. */
static int64_t user_difference(const ClockSnapshot *a, const ClockSnapshot *b) {
    if (memcmp(a->user.point.source, b->user.point.source, 16)) return 0;
    return (b->user.offset - a->user.offset) * 1000000000LL;
}

void *runtime_time_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn4time10InitializeEv", initialize}, {"_ZN2nn4time8FinalizeEv", finalize}, {"_ZN2nn4time13IsInitializedEv", is_initialized},
        {"_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", user_time},
        {"_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", user_time},
        {"_ZN2nn4time19StandardSteadyClock3nowEv", steady_clock_now},
        {"_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE", steady_time_point},
        {"_ZN2nn4time14GetSpanBetweenEPlRKNS0_20SteadyClockTimePointES4_", span_between_points},
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", to_calendar},
        {"_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", to_calendar_utc},
        {"_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", to_posix_utc},
        {"_ZN2nn4time13ClockSnapshotC1Ev", snapshot_init},
        {"_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", snapshot_create},
        {"_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", span_between},
        {"_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", user_difference},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
