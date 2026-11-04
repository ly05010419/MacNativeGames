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
#include <CoreFoundation/CoreFoundation.h>

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
static void adjustable_adjust(void) { /* The adjustable clock follows macOS wall time. */ }
static unsigned clock_context(SystemClockContext *out) {
 if(!out)return R_INVALID;SteadyClockTimePoint point=steady_now();
 *out=(SystemClockContext){posix_now()-point.value,point};return 0;
}
static unsigned device_location(char *out){if(!out)return R_INVALID;location_name(out);return 0;}
typedef struct {char magic[8],name[0x24];} HostZoneRule;
static CFTimeZoneRef rule_zone(const HostZoneRule *rule){
 if(!rule||memcmp(rule->magic,"MNGTZ001",8)||!memchr(rule->name,0,sizeof rule->name))return NULL;
 CFStringRef name=CFStringCreateWithCString(NULL,rule->name,kCFStringEncodingUTF8);
 if(!name)return NULL;CFTimeZoneRef zone=CFTimeZoneCreateWithName(NULL,name,false);CFRelease(name);return zone;
}
static unsigned load_zone_rule(void *output,const char *location){
 if(!output||!location||!memchr(location,0,0x24))return R_INVALID;
 HostZoneRule rule={0};memcpy(rule.magic,"MNGTZ001",8);strcpy(rule.name,location);
 CFTimeZoneRef zone=rule_zone(&rule);if(!zone)return R_INVALID;CFRelease(zone);
 memset(output,0,0x4000);memcpy(output,&rule,sizeof rule);return 0;
}
static unsigned calendar_with_rule(CalendarTime *calendar,CalendarAdditionalInfo *info,const int64_t *posix,const HostZoneRule *rule){
 if(!calendar||!posix)return R_INVALID;CFTimeZoneRef zone=rule_zone(rule);if(!zone)return R_INVALID;
 CFAbsoluteTime absolute=(double)*posix-kCFAbsoluteTimeIntervalSince1970;
 int32_t offset=(int32_t)CFTimeZoneGetSecondsFromGMT(zone,absolute);
 fill_calendar(calendar,info,*posix+offset,0);
 if(info){
  info->utc_offset=offset;info->dst=CFTimeZoneIsDaylightSavingTime(zone,absolute);
  CFStringRef abbreviation=CFTimeZoneCopyAbbreviation(zone,absolute);
  if(abbreviation){CFStringGetCString(abbreviation,info->name,sizeof info->name,kCFStringEncodingUTF8);CFRelease(abbreviation);}
 }
 CFRelease(zone);return 0;
}
static unsigned posix_with_rule(int *count,int64_t *out,int capacity,const CalendarTime *calendar,const HostZoneRule *rule){
 if(!count||!out||capacity<1||!calendar)return R_INVALID;
 CFTimeZoneRef zone=rule_zone(rule);if(!zone)return R_INVALID;
 int64_t naive=to_posix_utc(calendar);int n=0;
 for(int day=-2;day<=2;day++){
  CFAbsoluteTime when=(double)(naive+day*86400)-kCFAbsoluteTimeIntervalSince1970;
  int64_t candidate=naive-(int64_t)CFTimeZoneGetSecondsFromGMT(zone,when);
  CalendarTime check;unsigned result=calendar_with_rule(&check,NULL,&candidate,rule);
  if(result||memcmp(&check,calendar,7))continue;
  int duplicate=0;for(int i=0;i<n;i++)if(out[i]==candidate)duplicate=1;
  if(!duplicate&&n<capacity)out[n++]=candidate;
 }
 CFRelease(zone);*count=n;return 0;
}
static SystemClockContext snapshot_context(const ClockSnapshot *s){return s->user;}
static int64_t snapshot_posix(const ClockSnapshot *s){return s->user_time;}
static CalendarTime snapshot_calendar(const ClockSnapshot *s){return s->user_calendar;}

void *runtime_time_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn4time10InitializeEv", initialize}, {"_ZN2nn4time8FinalizeEv", finalize}, {"_ZN2nn4time13IsInitializedEv", is_initialized},
        {"_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", user_time},
        {"_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", user_time},
        {"_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", user_time},
        {"_ZN2nn4time25AdjustableUserSystemClock6AdjustEv", adjustable_adjust},
        {"_ZN2nn4time28AdjustableNetworkSystemClock6AdjustEv", adjustable_adjust},
        {"_ZN2nn4time23StandardUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", clock_context},
        {"_ZN2nn4time25AdjustableUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", clock_context},
        {"_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", device_location},
        {"_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", load_zone_rule},
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE", calendar_with_rule},
        {"_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", posix_with_rule},
        {"_ZN2nn4time19StandardSteadyClock3nowEv", steady_clock_now},
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", to_calendar},
        {"_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", to_calendar_utc},
        {"_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", to_posix_utc},
        {"_ZN2nn4time13ClockSnapshotC1Ev", snapshot_init},
        {"_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", snapshot_create},
        {"_ZN2nn4time13ClockSnapshot31CreateWithAdjustableSystemClockEPS1_", snapshot_create},
        {"_ZNK2nn4time13ClockSnapshot33GetStandardUserSystemClockContextEv", snapshot_context},
        {"_ZNK2nn4time13ClockSnapshot35GetStandardUserSystemClockPosixTimeEv", snapshot_posix},
        {"_ZNK2nn4time13ClockSnapshot38GetStandardUserSystemClockCalendarTimeEv", snapshot_calendar},
        {"_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", span_between},
        {"_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", user_difference},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
