/* SPDX-License-Identifier: MIT */
#ifndef TMNT_GRAPHICS_PHASE_H
#define TMNT_GRAPHICS_PHASE_H

#include <stdint.h>

/* The integrating runtime supplies these three host hooks. In particular,
 * core_away/core_back must not retain the global guest scheduler lock. */
_Noreturn void runtime_fail(const char *message);
int runtime_core_away(void);
void runtime_core_back(int *away);

void tmnt_graphics_owner_gate(unsigned operation, uint64_t *regs);
void tmnt_graphics_owner_thread_exit(void);
void tmnt_graphics_owner_mark_caught_return(void);
void tmnt_graphics_owner_finish_caught_return(void);
void tmnt_graphics_owner_abandon_on_rethrow(void);

/* Return 1 for a graphics-phase gate, 0 for an unrelated host gate.
 * The future TMNT runtime_dispatch calls this before its generic SVC path. */
int tmnt_graphics_owner_dispatch(unsigned operation, uint64_t *regs);

#endif
