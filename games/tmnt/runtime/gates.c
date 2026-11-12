#include "gates.h"
#include <stdlib.h>
#include <string.h>

struct TmntGateIndex { TmntGate *gates; size_t count; };

static int compare_gate(const void *a, const void *b) {
    uintptr_t x = ((const TmntGate *)a)->return_address;
    uintptr_t y = ((const TmntGate *)b)->return_address;
    return (x > y) - (x < y);
}

TmntGateStatus tmnt_gates_create(const TmntGate *gates, size_t count, TmntGateIndex **out) {
    if (!out || (count && !gates) || count > SIZE_MAX / sizeof *gates) return TMNT_GATE_ARGUMENT;
    *out = NULL;
    TmntGateIndex *index = calloc(1, sizeof *index);
    if (!index) return TMNT_GATE_MEMORY;
    if (count) {
        index->gates = malloc(count * sizeof *gates);
        if (!index->gates) { free(index); return TMNT_GATE_MEMORY; }
        memcpy(index->gates, gates, count * sizeof *gates);
    }
    index->count = count;
    for (size_t i = 0; i < count; ++i) {
        TmntGate g = index->gates[i];
        if (!g.return_address || g.return_address % 4 ||
            (g.kind != TMNT_GATE_SVC && g.kind != TMNT_GATE_COUNTER) ||
            (g.kind == TMNT_GATE_COUNTER && (g.argument >= 31 || g.argument == 18))) {
            tmnt_gates_destroy(index);
            return TMNT_GATE_ARGUMENT;
        }
    }
    if (count) qsort(index->gates, count, sizeof *gates, compare_gate);
    for (size_t i = 1; i < count; ++i) {
        if (index->gates[i - 1].return_address == index->gates[i].return_address) {
            tmnt_gates_destroy(index);
            return TMNT_GATE_DUPLICATE;
        }
    }
    *out = index;
    return TMNT_GATE_OK;
}

void tmnt_gates_destroy(TmntGateIndex *index) {
    if (index) { free(index->gates); free(index); }
}

TmntGateStatus tmnt_gate_dispatch(const TmntGateIndex *index, uintptr_t ret,
                                    uint64_t registers[31], TmntSvcHandler svc,
                                    TmntCounterHandler counter, void *context) {
    if (!index || !registers) return TMNT_GATE_ARGUMENT;
    TmntGate key = {ret, 0, 0};
    TmntGate *g = index->count ? bsearch(&key, index->gates, index->count, sizeof key, compare_gate) : NULL;
    if (!g) return TMNT_GATE_UNKNOWN;
    uint64_t copy[31];
    memcpy(copy, registers, sizeof copy);
    if (g->kind == TMNT_GATE_SVC) {
        if (!svc || svc(context, g->argument, copy) != 1) return TMNT_GATE_UNSUPPORTED;
    } else {
        uint64_t ticks;
        if (!counter || counter(context, &ticks) != 1) return TMNT_GATE_UNSUPPORTED;
        copy[g->argument] = ticks;
    }
    memcpy(registers, copy, sizeof copy);
    return TMNT_GATE_OK;
}
