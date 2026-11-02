#pragma once
#include <stdint.h>
/* Controls limit real game frames. Generated frames have their own switch and
   never silently halve the real frame cap. 0/0 means no application limit. */
static inline uint64_t totk_control_period(int vsync,int cap,int hz) {
 int rate=cap;
 if(vsync&&hz>0&&(!rate||hz<rate))rate=hz;
 return rate>0?1000000000ull/(unsigned)rate:0;
}
void nvn_control_pacing(int vsync,int cap);
int nvn_control_vsync(void); // -1: legacy/environment policy (CPU probes/iOS)
