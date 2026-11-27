/* Controller topology is independent of oe OperationMode / PerformanceMode.
   Portable/tabletop play can use four external full-key controllers without
   selecting docked graphics. Fixed at process startup, never switched live. */
#ifndef D3_CONTROLLER_TOPOLOGY_H
#define D3_CONTROLLER_TOPOLOGY_H
#include <stdlib.h>
#include <string.h>
static inline int d3_handheld_controller(void) {
    const char *mode = getenv("TOTK_CONTROLLER_MODE");
    return mode && !strcmp(mode, "fullkey") ? 0 : runtime_handheld_mode();
}
#endif
