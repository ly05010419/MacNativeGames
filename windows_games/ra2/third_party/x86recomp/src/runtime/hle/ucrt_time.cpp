// The UCRT's <time.h> for x64 guests. Times are __time64_t; the valid range
// is the UCRT's, 1970-01-01 00:00:00 to 3000-12-31 23:59:59 UTC (outside
// it the functions fail with EINVAL). Local time is the zone
// GetTimeZoneInformation reports: the host's current UTC offset, without
// DST (tm_isdst 0). gmtime and localtime share one struct tm per thread,
// ctime and asctime one string, as in the UCRT.

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/ucrt.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

constexpr int64_t kMaxTime64 = 32535215999;  // _MAX__TIME64_T: 3000-12-31 23:59:59
constexpr uint64_t kTmSize = 36;             // struct tm: nine ints

const char* const kDays[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                             "Thursday", "Friday", "Saturday"};
const char* const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

void Fail(int e) { Wr32(GuestErrno(), uint32_t(e)); }

int64_t LocalOffset() {
  std::time_t now = std::time(nullptr);
  std::tm t{};
  localtime_r(&now, &t);
  return t.tm_gmtoff;
}

void PutTm(gaddr p, const std::tm& t) {
  const int f[9] = {t.tm_sec,  t.tm_min,  t.tm_hour, t.tm_mday, t.tm_mon,
                    t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
  for (int i = 0; i < 9; i++) Wr32(p + 4 * i, uint32_t(f[i]));
}

std::tm GetTm(gaddr p) {
  std::tm t{};
  int* f[9] = {&t.tm_sec,  &t.tm_min,  &t.tm_hour, &t.tm_mday, &t.tm_mon,
               &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
  for (int i = 0; i < 9; i++) *f[i] = int32_t(Rd32(p + 4 * i));
  return t;
}

// Broken-down UTC time of t (in range).
std::tm Utc(int64_t t) {
  std::time_t h = std::time_t(t);
  std::tm r{};
  gmtime_r(&h, &r);
  return r;
}

std::tm Local(int64_t t) {
  std::tm r = Utc(t + LocalOffset());
  r.tm_isdst = 0;
  return r;
}

// The thread's struct tm (gmtime, localtime) and string (ctime, asctime).
gaddr TmBuffer() {
  static thread_local gaddr p = 0;
  if (!p) p = HeapAllocProcess(kTmSize, true);
  return p;
}
gaddr TextBuffer() {
  static thread_local gaddr p = 0;
  if (!p) p = HeapAllocProcess(26, true);
  return p;
}

// asctime's form, "Wed Jan 02 02:03:55 1980\n" (day of month two digits);
// false for fields out of range.
bool AscTime(const std::tm& t, char out[26]) {
  if (t.tm_sec < 0 || t.tm_sec > 59 || t.tm_min < 0 || t.tm_min > 59 || t.tm_hour < 0 ||
      t.tm_hour > 23 || t.tm_mday < 1 || t.tm_mday > 31 || t.tm_mon < 0 || t.tm_mon > 11 ||
      t.tm_year < 0 || t.tm_year > 8099 || t.tm_wday < 0 || t.tm_wday > 6)
    return false;
  std::snprintf(out, 26, "%.3s %.3s %02d %02d:%02d:%02d %04d\n", kDays[t.tm_wday],
                kMonths[t.tm_mon], t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, t.tm_year + 1900);
  return true;
}

}  // namespace

HLE64("ucrtbase.dll", _time64, kCdecl) {
  int64_t t = int64_t(std::time(nullptr));
  if (Arg(c, 0)) Wr64(Arg(c, 0), uint64_t(t));
  return uint64_t(t);
}

HLE64("ucrtbase.dll", _localtime64, kCdecl) {
  int64_t t = int64_t(Rd64(Arg(c, 0)));
  if (t < 0 || t > kMaxTime64) {
    Fail(EINVAL);
    return 0;
  }
  PutTm(TmBuffer(), Local(t));
  return TmBuffer();
}

// _gmtime64_s(tm*, const __time64_t*): 0 or EINVAL (the tm then all -1).
HLE64("ucrtbase.dll", _gmtime64_s, kCdecl) {
  gaddr out = Arg(c, 0), tp = Arg(c, 1);
  if (!out) {
    Fail(EINVAL);
    return EINVAL;
  }
  int64_t t = tp ? int64_t(Rd64(tp)) : -1;
  if (!tp || t < 0 || t > kMaxTime64) {
    for (int i = 0; i < 9; i++) Wr32(out + 4 * i, 0xFFFFFFFF);
    Fail(EINVAL);
    return EINVAL;
  }
  PutTm(out, Utc(t));
  return 0;
}

// _mkgmtime64(tm*): the tm as UTC, normalised in place (fields out of their
// ranges carry over, wday and yday are set); -1 out of range.
HLE64("ucrtbase.dll", _mkgmtime64, kCdecl) {
  gaddr p = Arg(c, 0);
  std::tm t = GetTm(p);
  int64_t r = int64_t(timegm(&t));
  if (r < 0 || r > kMaxTime64) {
    Fail(EINVAL);
    return uint64_t(-1);
  }
  t.tm_isdst = 0;
  PutTm(p, t);
  return uint64_t(r);
}

HLE64("ucrtbase.dll", _ctime64, kCdecl) {
  int64_t t = int64_t(Rd64(Arg(c, 0)));
  char s[26];
  if (t < 0 || t > kMaxTime64 || !AscTime(Local(t), s)) {
    Fail(EINVAL);
    return 0;
  }
  std::memcpy(GPtr(TextBuffer()), s, 26);
  return TextBuffer();
}

HLE64("ucrtbase.dll", asctime, kCdecl) {
  char s[26];
  if (!AscTime(GetTm(Arg(c, 0)), s)) {
    Fail(EINVAL);
    return 0;
  }
  std::memcpy(GPtr(TextBuffer()), s, 26);
  return TextBuffer();
}

// strftime(buf, max, format, tm): the conversions that do not depend on the
// locale or the time zone, and the "C" locale's names; others are fatal.
// 0 (and an empty buf, errno ERANGE) if the result does not fit.
HLE64("ucrtbase.dll", strftime, kCdecl) {
  gaddr buf = Arg(c, 0);
  uint64_t max = Arg(c, 1);
  std::string fmt = GStr(Arg(c, 2));
  std::tm t = GetTm(Arg(c, 3));
  std::string out;
  char num[16];
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') {
      out += fmt[i];
      continue;
    }
    char k = ++i < fmt.size() ? fmt[i] : 0;
    switch (k) {
      case 'Y': std::snprintf(num, sizeof num, "%d", t.tm_year + 1900); break;
      case 'y': std::snprintf(num, sizeof num, "%02d", (t.tm_year + 1900) % 100); break;
      case 'm': std::snprintf(num, sizeof num, "%02d", t.tm_mon + 1); break;
      case 'd': std::snprintf(num, sizeof num, "%02d", t.tm_mday); break;
      case 'H': std::snprintf(num, sizeof num, "%02d", t.tm_hour); break;
      case 'I': std::snprintf(num, sizeof num, "%02d", t.tm_hour % 12 ? t.tm_hour % 12 : 12); break;
      case 'M': std::snprintf(num, sizeof num, "%02d", t.tm_min); break;
      case 'S': std::snprintf(num, sizeof num, "%02d", t.tm_sec); break;
      case 'j': std::snprintf(num, sizeof num, "%03d", t.tm_yday + 1); break;
      case 'p': std::snprintf(num, sizeof num, "%s", t.tm_hour < 12 ? "AM" : "PM"); break;
      case '%': std::snprintf(num, sizeof num, "%%"); break;
      case 'a': case 'A': case 'b': case 'B': {
        bool day = k == 'a' || k == 'A';
        int idx = day ? t.tm_wday : t.tm_mon;
        if (idx < 0 || idx > (day ? 6 : 11)) FatalF("strftime: %%%c of an out-of-range tm", k);
        std::string name = day ? kDays[idx] : kMonths[idx];
        out += (k == 'a' || k == 'b') ? name.substr(0, 3) : name;
        continue;
      }
      default: FatalF("strftime: conversion %%%c not supported", k ? k : '0');
    }
    out += num;
  }
  if (out.size() + 1 > max) {
    if (max) Wr8(buf, 0);
    Fail(ERANGE);
    return 0;
  }
  PutStr(buf, out);
  return out.size();
}

}  // namespace recomp::hle
