#pragma once
// Minimal logging. Trace output (every HLE call) is enabled with
// <ID>_TRACE=1; warnings and fatal errors always print.

#include <cstdio>

namespace recomp {

extern bool g_trace;
// FatalF's report once the message is formatted (set by the runtime: guest
// state, backtraces, the crash file); must not return. Without one,
// FatalF prints the message and exits.
extern void (*g_fatal_report)(const char* msg);

[[noreturn]] void FatalF(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));
void WarnF(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void TraceF(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace recomp

#define RECOMP_TRACE(...)                    \
  do {                                    \
    if (::recomp::g_trace) ::recomp::TraceF(__VA_ARGS__); \
  } while (0)
