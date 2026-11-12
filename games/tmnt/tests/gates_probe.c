#include "gates.h"
#include <stdint.h>
#include <string.h>

static int svc(void *context, uint32_t number, uint64_t regs[31]) {
    (void)context;
    regs[0] = 99;
    return number == 7;
}
static int counter(void *context, uint64_t *ticks) {
    (void)context; *ticks = 12345; return 1;
}
int main(void) {
    TmntGate gates[] = {{0x3000, TMNT_GATE_COUNTER, 19}, {0x1000, TMNT_GATE_SVC, 7},
                         {0x2000, TMNT_GATE_SVC, 8}};
    TmntGateIndex *index = NULL;
    uint64_t regs[31] = {0}, before[31] = {0};
    if (tmnt_gates_create(gates, 3, &index) != TMNT_GATE_OK) return 1;
    gates[1].argument = 8; /* the index owns a copy */
    if (tmnt_gate_dispatch(index, 0x1000, regs, svc, counter, NULL) || regs[0] != 99) return 2;
    memcpy(before, regs, sizeof regs);
    if (tmnt_gate_dispatch(index, 0x2000, regs, svc, counter, NULL) != TMNT_GATE_UNSUPPORTED ||
        memcmp(before, regs, sizeof regs)) return 3;
    if (tmnt_gate_dispatch(index, 0x1004, regs, svc, counter, NULL) != TMNT_GATE_UNKNOWN ||
        memcmp(before, regs, sizeof regs)) return 4;
    if (tmnt_gate_dispatch(index, 0x3000, regs, svc, counter, NULL) || regs[19] != 12345) return 5;
    tmnt_gates_destroy(index);
    gates[1] = gates[0];
    if (tmnt_gates_create(gates, 3, &index) != TMNT_GATE_DUPLICATE || index) return 6;
    gates[1] = (TmntGate){0x4000, TMNT_GATE_COUNTER, 18};
    if (tmnt_gates_create(gates, 3, &index) != TMNT_GATE_ARGUMENT || index) return 7;
    if (tmnt_gates_create(NULL, 0, &index) != TMNT_GATE_OK) return 8;
    if (tmnt_gate_dispatch(index, 0x1000, regs, svc, counter, NULL) != TMNT_GATE_UNKNOWN) return 9;
    tmnt_gates_destroy(index);
    return 0;
}
