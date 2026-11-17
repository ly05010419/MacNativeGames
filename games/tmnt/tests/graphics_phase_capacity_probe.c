#include "graphics_phase.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Noreturn void runtime_fail(const char *message) {
    _Exit(strstr(message, "active device capacity exhausted") ? 7 : 9);
}
int runtime_core_away(void) { return 0; }
void runtime_core_back(int *away) { (void)away; }

int main(void) {
    uint32_t counts[33] = {0};
    for (int i = 0; i < 33; ++i) {
        uint64_t r[31] = {0}; r[8] = (uintptr_t)&counts[i]; r[0] = 1;
        tmnt_graphics_owner_gate(0x90, r);
        counts[i] = 1;
        tmnt_graphics_owner_gate(0x95, r);
    }
    return 1; /* A silent overwrite is a failure. */
}
