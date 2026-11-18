#ifndef TMNT_GATES_H
#define TMNT_GATES_H
#include <stddef.h>
#include <stdint.h>

typedef enum { TMNT_GATE_SVC = 1, TMNT_GATE_COUNTER = 2 } TmntGateKind;
typedef struct { uintptr_t return_address; uint32_t kind; uint32_t argument; } TmntGate;
typedef struct TmntGateIndex TmntGateIndex;
typedef int (*TmntSvcHandler)(void *context, uint32_t number, uint64_t registers[31]);
typedef int (*TmntCounterHandler)(void *context, uint64_t *ticks);
typedef enum {
    TMNT_GATE_OK = 0, TMNT_GATE_ARGUMENT, TMNT_GATE_MEMORY,
    TMNT_GATE_DUPLICATE, TMNT_GATE_UNKNOWN, TMNT_GATE_UNSUPPORTED
} TmntGateStatus;

/* Copies and validates the table; return addresses are exact host addresses. */
TmntGateStatus tmnt_gates_create(const TmntGate *gates, size_t count, TmntGateIndex **out);
void tmnt_gates_destroy(TmntGateIndex *index);
/* On failure, registers are unchanged. A handler returns 1 only on success.
   The counter callback must supply guest-frequency ticks, not host ticks. */
TmntGateStatus tmnt_gate_dispatch(const TmntGateIndex *index, uintptr_t return_address,
                                    uint64_t registers[31], TmntSvcHandler svc,
                                    TmntCounterHandler counter, void *context);
#endif
