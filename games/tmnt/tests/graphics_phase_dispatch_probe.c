#include "graphics_phase.h"
#include <stdint.h>
#include <stdlib.h>

_Noreturn void runtime_fail(const char *message) { (void)message; abort(); }
int runtime_core_away(void) { return 0; }
void runtime_core_back(int *away) { (void)away; }

int main(void) {
    int count = 0;
    uint64_t regs[31] = {0};
    regs[8] = (uintptr_t)&count;
    if (tmnt_graphics_owner_dispatch(0x80, regs)) return 1;
    if (!tmnt_graphics_owner_dispatch(0x90, regs)) return 2;
    count = 1;
    regs[0] = 1;
    if (!tmnt_graphics_owner_dispatch(0x95, regs)) return 3;
    if (!tmnt_graphics_owner_dispatch(0x92, regs)) return 4;
    regs[9] = 1;
    if (!tmnt_graphics_owner_dispatch(0x93, regs)) return 5;
    if (!tmnt_graphics_owner_dispatch(0x98, regs)) return 6;
    if (!tmnt_graphics_owner_dispatch(0x99, regs)) return 7;
    if (!tmnt_graphics_owner_dispatch(0x97, regs)) return 8;
    if (!tmnt_graphics_owner_dispatch(0x96, regs)) return 9;
    count = 0;
    regs[9] = 0;
    if (!tmnt_graphics_owner_dispatch(0x91, regs)) return 10;
    tmnt_graphics_owner_thread_exit();
    return 0;
}
