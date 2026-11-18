/* SPDX-License-Identifier: MIT
 * TMNT GraphicsDevice count-check gate. The phase covers exactly one guest
 * load/store, not managed rendering calls that can throw or unwind.
 */
#include "graphics_phase.h"
#include <pthread.h>
#include <stdint.h>

enum { PHASE_SET = 0x90, PHASE_PRESENT = 0x92, PHASE_CLEAR = 0x96 };
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t phase_owner;
static int phase_occupied;
static uintptr_t phase_device;
/* Several GraphicsDevice instances can be live during device replacement.
 * Keep active ownership per device; never silently overwrite another one. */
enum { TARGET_CAPACITY = 32 };
static struct target_state {
    uintptr_t device;
    pthread_t owner;
    int active;
} targets[TARGET_CAPACITY];
static __thread int local_phase;
static __thread int caught_return_pending;

static struct target_state *active_target(uintptr_t device) {
    for (unsigned i = 0; i < TARGET_CAPACITY; ++i)
        if (targets[i].active && targets[i].device == device) return &targets[i];
    return NULL;
}

static int target_blocks(uintptr_t device, int operation) {
    struct target_state *target = active_target(device);
    return operation != PHASE_CLEAR && target &&
           !pthread_equal(target->owner, pthread_self());
}

static void begin_phase(uintptr_t device, int operation) {
    if (!device || local_phase) runtime_fail("TMNT graphics phase: invalid entry");
    for (;;) {
        pthread_mutex_lock(&mu);
        if (!phase_occupied && !target_blocks(device, operation)) {
            phase_occupied = 1;
            phase_owner = pthread_self();
            phase_device = device;
            local_phase = operation;
            pthread_mutex_unlock(&mu);
            return;
        }
        if (phase_occupied && pthread_equal(phase_owner, pthread_self())) {
            pthread_mutex_unlock(&mu);
            runtime_fail("TMNT graphics phase: nested instruction");
        }
        pthread_mutex_unlock(&mu);
        /* The owner may need a guest core to reach its next instruction. */
        int away = runtime_core_away();
        pthread_mutex_lock(&mu);
        while (phase_occupied || target_blocks(device, operation))
            pthread_cond_wait(&cv, &mu);
        pthread_mutex_unlock(&mu);
        runtime_core_back(&away);
    }
}

static void end_phase(uintptr_t device, int operation, int count) {
    if (local_phase != operation) runtime_fail("TMNT graphics phase: unmatched exit");
    pthread_mutex_lock(&mu);
    if (!phase_occupied || !pthread_equal(phase_owner, pthread_self()) ||
        phase_device != device) {
        pthread_mutex_unlock(&mu);
        runtime_fail("TMNT graphics phase: wrong owner or device");
    }
    if (operation == PHASE_SET) {
        if (!count) {
            pthread_mutex_unlock(&mu);
            runtime_fail("TMNT graphics phase: zero target on set path");
        }
        struct target_state *target = active_target(device);
        if (!target) {
            for (unsigned i = 0; i < TARGET_CAPACITY; ++i)
                if (!targets[i].active) { target = &targets[i]; break; }
        }
        if (!target) {
            pthread_mutex_unlock(&mu);
            runtime_fail("TMNT graphics phase: active device capacity exhausted");
        }
        target->device = device;
        target->owner = pthread_self();
        target->active = 1;
    } else if (operation == PHASE_CLEAR) {
        if (count) {
            pthread_mutex_unlock(&mu);
            runtime_fail("TMNT graphics phase: clear wrote nonzero");
        }
        struct target_state *target = active_target(device);
        if (target) target->active = 0;
    }
    phase_occupied = 0;
    local_phase = 0;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

void tmnt_graphics_owner_gate(unsigned operation, uint64_t *regs) {
    uintptr_t device = (uintptr_t)regs[8];
    switch (operation) {
    case 0x90: begin_phase(device, PHASE_SET); return;      /* before STR */
    case 0x95: end_phase(device, PHASE_SET, (int)regs[0]); return;
    case 0x96: begin_phase(device, PHASE_CLEAR); return;    /* before STR */
    case 0x91: end_phase(device, PHASE_CLEAR, (int)regs[9]); return;
    case 0x92: begin_phase(device, PHASE_PRESENT); return;  /* before LDR */
    case 0x93: end_phase(device, PHASE_PRESENT, (int)regs[9]); return;
    default: runtime_fail("TMNT graphics phase: unknown gate");
    }
}

void tmnt_graphics_owner_thread_exit(void) {
    pthread_mutex_lock(&mu);
    if (local_phase && phase_occupied && pthread_equal(phase_owner, pthread_self())) {
        phase_occupied = 0;
        local_phase = 0;
    }
    /* A dead target writer cannot clear its count. Wake Present and let the
     * original managed count check report the still-active target. */
    for (unsigned i = 0; i < TARGET_CAPACITY; ++i)
        if (targets[i].active && pthread_equal(targets[i].owner, pthread_self()))
            targets[i].active = 0;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

/* The integrating runtime must call mark only after the caught handler's
 * calls, then finish only after its common state cleanup. Normal returns
 * pass finish without a mark and do not release a live target. */
void tmnt_graphics_owner_mark_caught_return(void) {
    caught_return_pending = 1;
}

static void abandon_current_target(void) {
    pthread_mutex_lock(&mu);
    struct target_state *owned = NULL;
    for (unsigned i = 0; i < TARGET_CAPACITY; ++i)
        if (targets[i].active && pthread_equal(targets[i].owner, pthread_self())) {
            if (owned) {
                pthread_mutex_unlock(&mu);
                runtime_fail("TMNT graphics phase: ambiguous exception device owner");
            }
            owned = &targets[i];
        }
    if (owned) {
        owned->active = 0;
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mu);
}

void tmnt_graphics_owner_finish_caught_return(void) {
    if (!caught_return_pending) return;
    caught_return_pending = 0;
    abandon_current_target();
}

void tmnt_graphics_owner_abandon_on_rethrow(void) {
    caught_return_pending = 0; /* A rethrow cannot leave a stale caught mark. */
    abandon_current_target();
}

int tmnt_graphics_owner_dispatch(unsigned operation, uint64_t *regs) {
    switch (operation) {
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x95: case 0x96:
        tmnt_graphics_owner_gate(operation, regs);
        return 1;
    case 0x97:
        tmnt_graphics_owner_abandon_on_rethrow();
        return 1;
    case 0x98:
        tmnt_graphics_owner_mark_caught_return();
        return 1;
    case 0x99:
        tmnt_graphics_owner_finish_caught_return();
        return 1;
    default:
        return 0;
    }
}
