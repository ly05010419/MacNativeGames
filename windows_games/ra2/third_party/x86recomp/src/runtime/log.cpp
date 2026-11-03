// runtime/log.h. In the base library (recomp_image), under the x87 model
// and the runtime, which both report through it.

#include "runtime/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace recomp {

bool g_trace = false;
void (*g_fatal_report)(const char* msg) = nullptr;

void FatalF(const char* fmt, ...) {
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  std::fflush(stdout);
  if (g_fatal_report) g_fatal_report(msg);
  std::fprintf(stderr, "FATAL: %s\n", msg);
  std::_Exit(3);
}

void WarnF(const char* fmt, ...) {
  std::fprintf(stderr, "warning: ");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

void TraceF(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

}  // namespace recomp
