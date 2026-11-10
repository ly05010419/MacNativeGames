/* nn::time for the functions main imports, backed by the host clock and time zone.
   User and network system clocks both follow the (NTP-synchronised) host wall clock; the steady clock
   counts seconds of host continuous time with one source id per process. ClockSnapshot uses the public
   0xD0-byte layout because game code may read its fields through inline accessors. */
#include "runtime.h"
#include <mach/mach_time.h>
#include <pthread.h>
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

static int64_t steady_ns(void) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    return (int64_t)((__uint128_t)mach_continuous_time() * tb.numer / tb.denom);
}
static int64_t steady_seconds(void) { return steady_ns() / 1000000000; }
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
#ifdef GAME_BOTW
/* SDK 7 (Breath of the Wild 1.6.0, botw.md): the steady clock is read into a time point, and a span is only defined between
   points of the same clock source. */
static uint32_t steady_time_point(SteadyClockTimePoint *out) { if (!out) runtime_fail("steady clock time point"); *out = steady_now(); return 0; }
static uint32_t span_between_points(int64_t *out, const SteadyClockTimePoint *a, const SteadyClockTimePoint *b) {
    if (!out || !a || !b) runtime_fail("time span between points");
    if (memcmp(a->source, b->source, 16)) return 0x2a6u;          /* time: not comparable (different clock sources) */
    *out = b->value - a->value; return 0;
}
#endif

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
static unsigned span_between(int64_t *out_ns, const ClockSnapshot *a, const ClockSnapshot *b) {
    if (!out_ns || !a || !b) return R_INVALID;
    if (memcmp(a->steady.source, b->steady.source, 16)) return R_SOURCE_MISMATCH;
    *out_ns = (b->steady.value - a->steady.value) * 1000000000LL; return 0;
}
/* Change of the user clock offset between snapshots, i.e. manual adjustments, as a TimeSpan. */
static int64_t user_difference(const ClockSnapshot *a, const ClockSnapshot *b) {
    if (memcmp(a->user.point.source, b->user.point.source, 16)) return 0;
    return (b->user.offset - a->user.offset) * 1000000000LL;
}

/* Time zone rules (SDK 20, Pokemon Pokopia): the device's location is the Mac's zone, and conversions with a rule use the
   Mac's local time as the rule-less ones do (the rule the game loads stays unused). */
void hle_signal_system_event(void *event);
static unsigned device_location(char *out) {
    if (!out) return R_INVALID;
    char link[256] = {0}; ssize_t n = readlink("/etc/localtime", link, sizeof link - 1);
    const char *zone = n > 0 && strstr(link, "zoneinfo/") ? strstr(link, "zoneinfo/") + 9 : "UTC";
    memset(out, 0, 0x24); snprintf(out, 0x24, "%s", zone); return 0;
}
static unsigned load_zone_rule(void *rule, const char *location) { (void)rule; (void)location; return 0; }
static unsigned to_calendar_rule(CalendarTime *c, CalendarAdditionalInfo *info, const int64_t *posix, const void *rule) { (void)rule; return to_calendar(c, info, posix); }
static unsigned to_posix_rule(int *count, int64_t *out, int n, const CalendarTime *c, const void *rule) {
    (void)rule; if (!c) return R_INVALID;
    struct tm tm = {0}; tm.tm_year = c->year - 1900; tm.tm_mon = c->month - 1; tm.tm_mday = c->day; tm.tm_hour = c->hour; tm.tm_min = c->minute; tm.tm_sec = c->second; tm.tm_isdst = -1;
    time_t v = mktime(&tm); if (count) *count = n > 0; if (n > 0 && out) out[0] = (int64_t)v; return 0;
}
static int64_t network_clock_now(void) { return (int64_t)time(NULL); }   /* StandardNetworkSystemClock::now: the Mac's clock, as GetCurrentTime */
/* AsyncEnsureNetworkClockAvailabilityTask: the network clock is the Mac's, available at once; its finish event is signalled. */
static uint8_t clock_task_event[0x80] __attribute__((aligned(16)));
static void clock_task_noop(void *self) { (void)self; }
static unsigned clock_task_ok(void *self) { (void)self; return 0; }
static void *clock_task_event_get(void *self) { (void)self; static int bound; if (!bound) { bound = 1; hle_bind_system_event(clock_task_event, 0); hle_signal_system_event(clock_task_event); } return clock_task_event; }

/* TOTK_CLOCK_POLL_LOG=1 (diagnostic, 2026-11-08): which guest threads poll the clock, from where, how often. The Time
   Profiler showed one guest thread on a P-core 90% of every Ocarina frame with nothing but clock_gettime calls into the
   runtime: gRPC's timer thread looping because the SDK's pthread_cond_timedwait failed (see the POSIX clock providers below).
   The guest return address comes from the frame chain, which the HLE stack switch links back to the guest caller
   (hle_stack.S). Every 2 s: calls per second per (host thread id, guest caller, its caller). */
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);
typedef struct { uint64_t tid, lr, lr2, n; const char *mod, *mod2; uint64_t off, off2; } clock_poll_ent;
static void clock_poll_note(void) {
 static int on = -1; if (on < 0) { const char *e = getenv("TOTK_CLOCK_POLL_LOG"); on = e && atoi(e); }
 if (!on) return;
 uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
 uint64_t lr = 0, off = 0, lr2 = 0, off2 = 0; const char *mod = NULL, *mod2 = NULL;
 uintptr_t *fp = __builtin_frame_address(0);
 for (int d = 0; d < 24 && fp; d++) {
  uintptr_t ret = fp[1];
  if (runtime_module_offset(ret, &mod, &off)) {
   /* The host frame record of the HLE stack switch: [fp] = guest x29/x30, [fp+16] = guest sp at the call (hle_stack.S).
      The guest return address is inside the SDK's clock_gettime (sdk+0xb52a0: `stp x29,x30,[sp,#-0x20]!` ... `bl sdk+0xaf160`,
      which jumps through the provider slot to the host), so the caller that wants the time left its return address at
      [gsp+8], clock_gettime's saved x30. Recorded only when it is a code address of a loaded module. */
   lr = ret;
   /* Only the switch frame has [fp+16] = guest sp and [fp+24] = this host stack's top (just above this frame); a nested
      host call or a host thread reaches guest code through other frames whose words are not a stack pointer. */
   uint64_t gsp = fp[2], top = fp[3];
   if (top > (uintptr_t)fp && top - (uintptr_t)fp < (1u << 22) && gsp > 0x100000 && !(gsp & 15) && gsp + 0x20 > gsp) {
    const char *m2; uint64_t o2, cand = *(uint64_t *)(gsp + 8);
    if (runtime_module_offset(cand, &m2, &o2)) { lr2 = cand; mod2 = m2; off2 = o2; }
   }
   break;
  }
  uintptr_t *next = (uintptr_t *)fp[0];
  if (next <= fp || ((uintptr_t)next & 7)) break;
  fp = next;
 }
 static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
 static clock_poll_ent ent[64]; static int nent; static uint64_t since, total;
 pthread_mutex_lock(&mu);
 int i; for (i = 0; i < nent; i++) if (ent[i].tid == tid && ent[i].lr == lr && ent[i].lr2 == lr2) break;
 if (i == nent && nent < 64) { ent[nent] = (clock_poll_ent){tid, lr, lr2, 0, mod, mod2, off, off2}; nent++; }
 if (i < nent) ent[i].n++;
 total++;
 uint64_t now = runtime_now_ns(); if (!since) since = now;
 if (now - since >= 2000000000ull) {
  double s = (now - since) / 1e9;
  fprintf(stderr, "[totk-clock] %.0f clock_gettime calls/s from %d (thread, caller) pairs\n", total / s, nent);
  for (int k = 0; k < 6; k++) {
   int best = -1; for (int j = 0; j < nent; j++) if (ent[j].n && (best < 0 || ent[j].n > ent[best].n)) best = j;
   if (best < 0) break;
   fprintf(stderr, "[totk-clock]   thread 0x%llx: %.0f/s from %s+0x%llx <- %s+0x%llx\n", (unsigned long long)ent[best].tid, ent[best].n / s, ent[best].mod ? ent[best].mod : "?", (unsigned long long)ent[best].off, ent[best].mod2 ? ent[best].mod2 : "?", (unsigned long long)ent[best].off2);
   ent[best].n = 0;
  }
  for (int j = 0; j < nent; j++) ent[j].n = 0;
  total = 0; since = now;
 }
 pthread_mutex_unlock(&mu);
}
/* The SDK libc's POSIX clocks (2026-11-08). The SDK's clock_gettime(CLOCK_REALTIME / CLOCK_MONOTONIC) jumps through two
   function pointers that the SDK fills (relative relocations, redirected here by loader.c relocate) with
   nn::time::GetStandardUserSystemClockCurrentTimespec / GetStandardSteadyClockCurrentTimespec. The SDK's own versions need
   the nn::time shared-memory client that its nn::time::Initialize sets up, which we replace: they answered result 0x74,
   clock_gettime failed with EAGAIN, the SDK's pthread_cond_timedwait returned 11 at once, and gRPC's timer thread (which takes
   11 as a wake-up) spun on one core at up to 6 M waits/s. The whole process now has one clock_gettime, the SDK's, backed by
   these two: wall clock = the host's CLOCK_REALTIME (as GetCurrentTime above, with nanoseconds), steady = host continuous time
   (as steady_seconds). main's import of clock_gettime is no longer replaced separately (it was, with the host clock, which hid
   the failing SDK path from direct callers only). A timespec is two int64 words in the SDK as on Darwin arm64. */
static uint32_t user_clock_timespec(int64_t *ts) {
    runtime_core_checkpoint(); clock_poll_note();
    if (!ts) return R_INVALID;
    struct timespec t; if (clock_gettime(CLOCK_REALTIME, &t)) return R_INVALID;
    ts[0] = t.tv_sec; ts[1] = t.tv_nsec; return 0;
}
static uint32_t steady_clock_timespec(int64_t *ts) {
    runtime_core_checkpoint(); clock_poll_note();
    if (!ts) return R_INVALID;
    int64_t ns = steady_ns(); ts[0] = ns / 1000000000; ts[1] = ns % 1000000000; return 0;
}
void *runtime_time_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn4time41GetStandardUserSystemClockCurrentTimespecEP8timespec", user_clock_timespec},
        {"_ZN2nn4time43GetAdjustableUserSystemClockCurrentTimespecEP8timespec", user_clock_timespec},   /* the libc's other CLOCK_REALTIME source (sdk+0xaf020 kind 2) */
        {"_ZN2nn4time37GetStandardSteadyClockCurrentTimespecEP8timespec", steady_clock_timespec},
        {"_ZN2nn4time10InitializeEv", initialize}, {"_ZN2nn4time8FinalizeEv", finalize}, {"_ZN2nn4time13IsInitializedEv", is_initialized},
        {"_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", user_time},
        {"_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", user_time},
        {"_ZN2nn4time19StandardSteadyClock3nowEv", steady_clock_now},
#ifdef GAME_BOTW
        {"_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE", steady_time_point},
        {"_ZN2nn4time14GetSpanBetweenEPlRKNS0_20SteadyClockTimePointES4_", span_between_points},
#endif
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", to_calendar},
        {"_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", to_calendar_utc},
        {"_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", device_location},
        {"_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", load_zone_rule},
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE", to_calendar_rule},
        {"_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", to_posix_rule},
        {"_ZN2nn4time26StandardNetworkSystemClock3nowEv", network_clock_now},
        {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", clock_task_noop}, {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", clock_task_noop},
        {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", clock_task_ok},
        {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", clock_task_ok},
        {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", clock_task_ok},
        {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", clock_task_event_get},
        {"_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", to_posix_utc},
        {"_ZN2nn4time13ClockSnapshotC1Ev", snapshot_init},
        {"_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", snapshot_create},
        {"_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", span_between},
        {"_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", user_difference},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
