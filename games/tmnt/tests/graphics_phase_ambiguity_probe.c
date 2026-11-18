#include "graphics_phase.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Noreturn void runtime_fail(const char *message) {
    _Exit(strstr(message, "ambiguous exception device owner") ? 8 : 9);
}
int runtime_core_away(void) { return 0; }
void runtime_core_back(int *away) { (void)away; }

int main(void) {
    uint32_t a = 0, b = 0;
    uint64_t r[31] = {0}; r[0] = 1;
    r[8] = (uintptr_t)&a;
    tmnt_graphics_owner_gate(0x90, r); a = 1;
    tmnt_graphics_owner_gate(0x95, r);
    r[8] = (uintptr_t)&b;
    tmnt_graphics_owner_gate(0x90, r); b = 1;
    tmnt_graphics_owner_gate(0x95, r);
    tmnt_graphics_owner_abandon_on_rethrow();
    return 1; /* Neither device may be silently released. */
}
