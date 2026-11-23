/* System services libnx's start reaches: set:sys (the firmware version, which picks libnx's command paths), set (the
   language), time:u (the Mac's clock and time zone). Layouts follow switchbrew's "Settings services" and "Glue services"
   pages and libnx's set.c / time.c. */
#include "hb.h"
#include <time.h>

typedef struct {
    uint8_t major, minor, micro, pad0, revision_major, revision_minor, pad1[2];
    char platform[0x20], version_hash[0x40], display_version[0x18], display_title[0x80];
} FirmwareVersion;
_Static_assert(sizeof(FirmwareVersion) == 0x100, "SetSysFirmwareVersion");

void hb_firmware_version(FirmwareVersion *v) {
    memset(v, 0, sizeof *v);
    v->major = HB_FW_MAJOR; v->minor = HB_FW_MINOR; v->micro = HB_FW_MICRO;
    snprintf(v->platform, sizeof v->platform, "NX");
    snprintf(v->display_version, sizeof v->display_version, "%u.%u.%u", HB_FW_MAJOR, HB_FW_MINOR, HB_FW_MICRO);
    snprintf(v->display_title, sizeof v->display_title, "NintendoSDK Firmware for NX %u.%u.%u", HB_FW_MAJOR, HB_FW_MINOR, HB_FW_MICRO);
}
static void set_sys(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 3: case 4: { FirmwareVersion v; hb_firmware_version(&v); hb_write_out(rq, 0, &v, sizeof v); return; }   /* GetFirmwareVersion(2) */
    default: HB_UNKNOWN(s, rq);
    }
}
/* The language the game is told: Traditional Chinese, the language of 明星三缺一's own text (Big5). */
static const char LANGUAGE[8] = "zh-Hant";
static void set(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { uint64_t v = 0; memcpy(&v, LANGUAGE, 8); IPC_OUT(rs, v); return; }        /* GetLanguageCode */
    case 1: case 5: {                                                                   /* GetAvailableLanguageCodes(2) */
        uint64_t v = 0; memcpy(&v, LANGUAGE, 8); hb_write_out(rq, 0, &v, 8); HB_OUT_U32(rs, 1); return; }
    case 2: { uint64_t v = 0; memcpy(&v, LANGUAGE, 8); IPC_OUT(rs, v); return; }        /* MakeLanguageCode: the one language offered */
    case 3: case 6: HB_OUT_U32(rs, 1); return;                                          /* GetAvailableLanguageCodeCount(2) */
    case 4: HB_OUT_U32(rs, 4); return;                                                  /* GetRegionCode: China/Taiwan (HTK) */
    default: HB_UNKNOWN(s, rq);
    }
}

/* ---------- time ---------- */
static void system_clock(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: HB_OUT_U64(rs, (uint64_t)time(NULL)); return;                               /* GetCurrentTime */
    case 2: { uint8_t context[0x20] = {0}; uint64_t now = (uint64_t)time(NULL); memcpy(context, &now, 8); ipc_out(rs, context, sizeof context); return; }   /* GetSystemClockContext */
    default: HB_UNKNOWN(s, rq);
    }
}
static void steady_clock(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { struct { int64_t seconds; uint8_t source[16]; } p = {(int64_t)(runtime_now_ns() / 1000000000ull), {1}}; IPC_OUT(rs, p); return; }   /* GetCurrentTimePoint */
    default: HB_UNKNOWN(s, rq);
    }
}
typedef struct { uint16_t year; uint8_t month, day, hour, minute, second, pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char name[8]; uint32_t is_dst; int32_t offset; } CalendarInfo;
_Static_assert(sizeof(CalendarTime) == 8 && sizeof(CalendarInfo) == 0x18, "calendar types");
/* The Mac's local time of a POSIX time, as ToCalendarTimeWithMyRule gives it. */
void hb_calendar(int64_t posix, CalendarTime *ct, CalendarInfo *ci) {
    time_t t = (time_t)posix; struct tm tm; localtime_r(&t, &tm);
    *ct = (CalendarTime){(uint16_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1), (uint8_t)tm.tm_mday, (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec, 0};
    memset(ci, 0, sizeof *ci);
    ci->wday = (uint32_t)tm.tm_wday; ci->yday = (uint32_t)tm.tm_yday; ci->is_dst = tm.tm_isdst > 0; ci->offset = (int32_t)tm.tm_gmtoff;
    snprintf(ci->name, sizeof ci->name, "%.7s", tm.tm_zone ? tm.tm_zone : "UTC");
}
static void time_zone(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { char name[0x24] = "Asia/Taipei"; const char *tz = getenv("TZ"); if (tz && *tz) snprintf(name, sizeof name, "%s", tz); ipc_out(rs, name, sizeof name); return; }   /* GetDeviceLocationName */
    case 101: {                                                                         /* ToCalendarTimeWithMyRule */
        CalendarTime ct; CalendarInfo ci; hb_calendar(IPC_ARG(rq, int64_t, 0), &ct, &ci);
        IPC_OUT(rs, ct); IPC_OUT(rs, ci); return; }
    case 201: case 202: {                                                               /* ToPosixTime(rule) / ToPosixTimeWithMyRule */
        CalendarTime ct = IPC_ARG(rq, CalendarTime, 0);
        struct tm tm = {.tm_year = ct.year - 1900, .tm_mon = ct.month - 1, .tm_mday = ct.day, .tm_hour = ct.hour, .tm_min = ct.minute, .tm_sec = ct.second, .tm_isdst = -1};
        int64_t posix = (int64_t)mktime(&tm); hb_write_out(rq, 0, &posix, 8); HB_OUT_U32(rs, 1); return; }
    default: HB_UNKNOWN(s, rq);
    }
}
static void time_service(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: hb_session(rs, "ISystemClock:user", system_clock, NULL, NULL); return;     /* GetStandardUserSystemClock */
    case 1: hb_session(rs, "ISystemClock:network", system_clock, NULL, NULL); return;  /* GetStandardNetworkSystemClock */
    case 2: hb_session(rs, "ISteadyClock", steady_clock, NULL, NULL); return;          /* GetStandardSteadyClock */
    case 3: hb_session(rs, "ITimeZoneService", time_zone, NULL, NULL); return;         /* GetTimeZoneService */
    case 4: hb_session(rs, "ISystemClock:local", system_clock, NULL, NULL); return;    /* GetStandardLocalSystemClock */
    default: HB_UNKNOWN(s, rq);
    }
}
void hb_system_register(void) {
    runtime_ipc_register("set:sys", set_sys, NULL, NULL);
    runtime_ipc_register("set", set, NULL, NULL);
    runtime_ipc_register("time:u", time_service, NULL, NULL);
    runtime_ipc_register("time:s", time_service, NULL, NULL);
}
