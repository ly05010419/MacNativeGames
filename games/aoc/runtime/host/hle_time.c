// nn::time: a real device gets the network-calibrated user clock through the time service; the host reads the system clock directly (single machine, offline).
// Layout follows the public Horizon ABI: PosixTime = int64 seconds; CalendarTime = {s16 year, s8 month/day/hour/minute/second};
// CalendarAdditionalInfo = {u32 day of week, u32 day of year, char time zone name[8], u32 DST, s32 UTC offset in seconds}.
#include "hle.h"
#include <stdatomic.h>
#include <time.h>

typedef struct { int16_t year; int8_t month, day, hour, minute, second; int8_t pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char tzname[8]; uint32_t dst; int32_t offset; } CalendarAdditionalInfo;

// DM_FIXED_TIME=seconds (for deterministic comparison): the user clock always returns this value (and main.c pins the host time zone to UTC)
static int64_t fixed_time(void) {
    static int64_t v = -2;
    if (v == -2) { const char *e = getenv("DM_FIXED_TIME"); v = e ? strtoll(e, 0, 0) : -1;
                   if (v >= 0) LOG("DM_FIXED_TIME=%lld: the user clock returns a fixed value", (long long)v); }
    return v;
}
HLE_FORCE("_ZN2nn4time10InitializeEv", "connect to the time service; clocks come from the host system time instead", Result, h_time_Initialize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4time13IsInitializedEv", "the time service is taken over, always initialized", int, h_time_IsInitialized, void) { return 1; }
// Steady clock (seconds since boot + a clock source ID, a 16-byte UUID): the host's monotonic seconds with a fixed source ID. GetSpanBetween runs as is in sdk: it subtracts only when the source IDs match
HLE_FORCE("_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE", "steady clock → host monotonic clock (seconds), fixed source ID", Result, h_time_SteadyNow, uint8_t *out) {
    int64_t s = (int64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000ull);
    memcpy(out, &s, 8); memcpy(out + 8, "dmhost-steady-01", 16); return RES_OK;
}
HLE_FORCE("_ZN2nn4time38GetStandardSteadyClockCurrentTimePointEPNS0_20SteadyClockTimePointE", "steady clock (the free function form, SDK 20) → host monotonic clock", Result, h_time_SteadyNowFn, uint8_t *out) { return h_time_SteadyNow(out); }
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "user clock → host time()", Result, h_time_GetCurrentTime, int64_t *out) {
    int64_t ft = fixed_time(); *out = ft >= 0 ? ft : time(NULL); return RES_OK;
}
// The adjustable user clock (Animal Crossing: New Horizons 3.0.3, whose days follow it): the SDK keeps it in a MonotonicSystemClock that Adjust sets from the user clock's context, and its
// GetCurrentTime reads the time service's shared memory, which the host never maps (an abort in MonotonicSystemClock::GetCurrentTime, sdk+0x4421cc on that build). On the host the user clock
// is the host's own clock, so it is this one too; Adjust runs natively (it reads the user clock context, a host take-over) and only sets the SDK's copy
HLE_FORCE("_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "adjustable user clock → host time(), as the user clock", Result, h_time_AdjGetCurrentTime, int64_t *out) { return h_time_GetCurrentTime(out); }
// The C library's real-time clock (SDK 17): clock_gettime(CLOCK_REALTIME) calls the source that nntimeStartup selects once time::Initialize succeeds (sdk+0x8e414: 1 = the standard user clock,
// 2 = the adjustable one; 0, the system tick, before that), and std::chrono::system_clock::now() throws on its failure. The SDK's getters ask the time service: Ace Attorney Investigations
// Collection called std::terminate at its first real-time read, right after the first-boot language dialog (main+0x3f3f68; found by armyja, GitHub issue #18). The host user clock, to the nanosecond
static int h_time_timespec(int64_t *ts) {
    int64_t ft = fixed_time();
    if (ft >= 0) { ts[0] = ft; ts[1] = 0; return 0; }
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t); ts[0] = t.tv_sec; ts[1] = t.tv_nsec; return 0;
}
HLE_FORCE("_ZN2nn4time41GetStandardUserSystemClockCurrentTimespecEP8timespec", "C library real-time clock from the user clock → host real time", int, h_time_StdTimespec, int64_t *ts) { return h_time_timespec(ts); }
HLE_FORCE("_ZN2nn4time43GetAdjustableUserSystemClockCurrentTimespecEP8timespec", "C library real-time clock from the adjustable user clock → host real time", int, h_time_AdjTimespec, int64_t *ts) { return h_time_timespec(ts); }
// User clock context (0x20 bytes: s64 offset + the steady time point it was made at; user time = offset + a steady time point of the same source ID, the snapshot below fills its +0x00 the same way).
// The SDK reads it from the time service's shared memory, which the host never opens: it returns 0x74 and writes nothing. Unity's DateTime.UtcNow (Engage main+0x4ad40c; Ace Attorney Investigations
// 1&2 per PR #14) then never starts its clock and reads 0001-01-01. Unity takes its time from GetCurrentTime plus system ticks and only compares the context to notice the clock being set, so like a
// Switch's it stays the same until the host clock moves against the guest ticks by more than a second (clock set by hand, a sleep mach_absolute_time did not count); with DM_FIXED_TIME it is made once
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", "user clock context from the time service; the host's user clock over its steady clock, renewed when the host clock is set",
          Result, h_time_GetContext, uint8_t *out) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static uint8_t ctx[0x20]; static int64_t drift; static int made;
    uint8_t tp[0x18]; int64_t now; h_time_SteadyNow(tp); h_time_GetCurrentTime(&now);
    int64_t d = now - (int64_t)(guest_ns() / 1000000000ull);
    pthread_mutex_lock(&mu);
    if (!made || (fixed_time() < 0 && llabs(d - drift) > 1)) {
        int64_t off = now - *(const int64_t *)tp; memcpy(ctx, &off, 8); memcpy(ctx + 8, tp, 0x18);
        if (made) LOG("time: host clock moved %+lld s against the guest ticks, new user clock context", (long long)(d - drift));
        drift = d; made = 1;
    }
    memcpy(out, ctx, 0x20);
    pthread_mutex_unlock(&mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock3nowEv", "user clock → host time()", int64_t, h_time_now, void) {
    int64_t ft = fixed_time(); return ft >= 0 ? ft : time(NULL); }
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock9to_time_tERKNSt3__16chrono10time_pointINS0_17SystemClockTraitsENS3_8durationIlNS2_5ratioILl1ELl1EEEEEEE",
          "the SDK implementation pairs with now(), taken over with it", int64_t, h_time_to_time_t, const int64_t *tp) { return *tp; }
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
          "time zone rules of the time service; uses the host localtime_r instead", Result, h_time_ToCalendarTime, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt) {
    time_t t = *pt; struct tm tm; localtime_r(&t, &tm);
    static int once; if (!once++) LOG("ToCalendarTime: first conversion %lld → %04d-%02d-%02d %02d:%02d (time zone %s, offset %ld s)", (long long)t, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_zone ? tm.tm_zone : "?", (long)tm.tm_gmtoff);
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) { memset(ai, 0, sizeof *ai); ai->wday = tm.tm_wday; ai->yday = tm.tm_yday; strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname);
              ai->dst = tm.tm_isdst > 0; ai->offset = (int32_t)tm.tm_gmtoff; }
    return RES_OK;
}
// Explicit time zone rules (Monster Hunter Rise, SDK 11): LoadTimeZoneRule reads the rule from the time service; the host keeps the game's rule buffer untouched and converts with localtime_r as above
HLE_FORCE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", "time zone rule from the time service; the host converts with its own zone, the rule stays unused", Result, h_time_LoadRule,
          void *rule, const char *name) { (void)rule; LOG("time::LoadTimeZoneRule %.36s (host time zone used instead)", name ? name : "?"); return RES_OK; }
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE", "conversion with an explicit time zone rule; same as the device zone (host localtime_r)",
          Result, h_time_ToCalendarTimeRule, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt, const void *rule) { (void)rule; return h_time_ToCalendarTime(ct, ai, pt); }
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", "network clock accuracy (time service): offline, never sufficient", unsigned, h_time_NetAccurate, void) { return 0; }
// Clock snapshot (Tears of the Kingdom: CreateWithStandardSystemClock asks the time service, which the host never opens → abort in sm::Initialize). 0xD0 bytes, laid out
// as Ryujinx lays out the service's ClockSnapshot and as this SDK's constructor clears it (its last store is the byte at +0xcd): +0x00 user clock context {offset,
// steady time point}, +0x20 network clock context, +0x40 user time, +0x48 network time, +0x50 / +0x58 user / network calendar, +0x60 / +0x78 their additional info,
// +0x90 steady time point, +0xa8 location name (0x24), +0xcc automatic correction, +0xcd type. A steady time point is {int64 seconds, 16-byte source ID} (h_time_SteadyNow).
// Offline, the network fields stay 0, so CalculateSpanBetween (below) uses the steady time points (same source ID)
HLE_FORCE("_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", "snapshot from the time service; the host fills it on the spot (offline: network clock 0)", Result, h_time_Snapshot, uint8_t *s) {
    memset(s, 0, 0xd0);
    uint8_t tp[0x18]; h_time_SteadyNow(tp);
    int64_t now; h_time_GetCurrentTime(&now);
    h_time_GetContext(s);   // +0x00: the user clock context, the same in every snapshot until the host clock is set (worked out per snapshot it moved by a second at random, a "clock change" to the user difference below)
    memcpy(s + 0x40, &now, 8);
    h_time_ToCalendarTime((CalendarTime *)(s + 0x50), (CalendarAdditionalInfo *)(s + 0x60), &now);
    memcpy(s + 0x90, tp, 0x18);
    tzset(); strlcpy((char *)s + 0xa8, tzname[0] && *tzname[0] ? tzname[0] : "UTC", 0x24);
    return RES_OK;
}
// Arithmetic on two snapshots: the SDK hands both to the time service (a call through its session, AbortImpl when there is none: STEINS;GATE RE:BOOT's and Tears of the Kingdom's SDK;
// Tears of the Kingdom imports both); the host computes what the service does (as Ryujinx's time service). TimeSpan is nanoseconds
static int64_t snap64(const uint8_t *s, int off) { int64_t v; memcpy(&v, s + off, 8); return v; }
static int same_source(const uint8_t *a, const uint8_t *b, int tp) { return !memcmp(a + tp + 8, b + tp + 8, 16); }   // the source IDs of the steady time points at +tp
HLE_FORCE("_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", "span between two snapshots (time service): their steady time points, else their network times",
          Result, h_time_SpanBetween, int64_t *out, const uint8_t *a, const uint8_t *b) {
    int64_t s;
    if (same_source(a, b, 0x90)) s = snap64(b, 0x90) - snap64(a, 0x90);
    else if (snap64(a, 0x48) && snap64(b, 0x48)) s = snap64(b, 0x48) - snap64(a, 0x48);
    else return RES_FAIL;   // no common clock: the service answers "not found", *out untouched (as the SDK leaves it)
    *out = s * 1000000000; return RES_OK;
}
// How far the user moved the user clock between two snapshots (asked at Pokémon: Let's Go's first save, PR #36): the change of the user clock offset, 0 when the two contexts' steady
// sources differ or both snapshots have automatic correction on. The snapshots carry h_time_GetContext's context, renewed when the host clock is set, so that shows up here
HLE_FORCE("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", "user clock change between two snapshots (time service): from their user clock offsets",
          int64_t, h_time_UserDiff, const uint8_t *a, const uint8_t *b) {
    if (!same_source(a, b, 0x08) || (a[0xcc] && b[0xcc])) return 0;
    return (snap64(b, 0x00) - snap64(a, 0x00)) * 1000000000;
}
// The reverse: local calendar → absolute seconds (13 Sentinels). Around a daylight-saving switch one local time maps to 0 or 2 absolute seconds; the host's mktime gives one
HLE_FORCE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE",
          "time zone rules of the time service; uses the host mktime instead", Result, h_time_ToPosixTime, int *count, int64_t *out, int n, const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second, .tm_isdst = -1};
    time_t t = mktime(&tm);
    if (count) *count = n > 0;
    if (n > 0 && out) out[0] = (int64_t)t;
    return RES_OK;
}
// With an explicit time zone rule (Pokémon Legends: Z-A, SDK 19; the rule came from the host's LoadTimeZoneRule above and stays unused): the device zone, as for ToCalendarTime
HLE_FORCE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", "conversion with an explicit time zone rule; same as the device zone (host mktime)",
          Result, h_time_ToPosixTimeRule, int *count, int64_t *out, int n, const CalendarTime *ct, const void *rule) { (void)rule; return h_time_ToPosixTime(count, out, n, ct); }
// UTC calendar → absolute seconds (Super Mario Odyssey); no time zone involved, the host's timegm
HLE_FORCE("_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", "UTC calendar → absolute seconds; host timegm", int64_t, h_time_ToPosixUtc, const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second};
    return (int64_t)timegm(&tm);
}
// Local time conversion is forced onto the host libc: the SDK's own localtime / localtime_r / mktime have no time zone data and always use UTC
// (a game's local timestamps were off by the time zone's offset). The SDK on a real Switch converts by the system time zone, so only following the host system time zone here is consistent;
// the host TZ normally = the system time zone, and is pinned to UTC only for deterministic comparison with DM_FIXED_TIME (main.c). The struct tm layout is the same on both sides (9 ints, long offset, time zone name pointer).
// gmtime / gmtime_r / timegm are UTC anyway and are not taken over. The same-named pass-through entries in hle_table.c only take effect when the symbol has no definition (the SDK defines them), hence HLE_FORCE
HLE_FORCE("localtime", "local time → host localtime (follows the system time zone)", struct tm *, h_localtime, const int64_t *t) { time_t v = (time_t)*t; return localtime(&v); }
HLE_FORCE("localtime_r", "local time → host localtime_r (follows the system time zone)", struct tm *, h_localtime_r, const int64_t *t, struct tm *out) { time_t v = (time_t)*t; return localtime_r(&v, out); }
HLE_FORCE("mktime", "local calendar → absolute seconds, host mktime (follows the system time zone)", int64_t, h_mktime, struct tm *tm) { return (int64_t)mktime(tm); }
