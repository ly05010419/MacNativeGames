/* Animal Crossing: New Horizons only -- thread diagnostics for stack-hosted command buffers, included by threads.c after
   the thread table. ACNH_STACKCB=1 turns them on; otherwise every hook returns at its first test and nothing changes.

   Why (2026-10-25, TOTK_CB_CHECK run): a worker thread called EndRecording on a command buffer that lived in the main thread's
   stack frame after the main thread had left that frame -- the memory held floats and stack pointers. These record, per guest
   thread, its last core events (got a core, gave it up, was preempted, read its core number), and let the command buffer
   guards (gfx/nvn/acnh_cmd_guard.h) find which thread's stack an address is on and where that thread is right now. */
#ifndef ACNH_THREAD_DIAG_H
#define ACNH_THREAD_DIAG_H
#include "game_version.h"   /* GAME_ACNH: the runtime is built without the graphics layer's -DGAME_ACNH */
#ifdef GAME_ACNH
#include <pthread.h>

enum { ACNH_EV_TAKE = 'T', ACNH_EV_GRANT = 'G', ACNH_EV_STARVE = 'S', ACNH_EV_PUT = 'P', ACNH_EV_PREEMPT = 'X', ACNH_EV_READ = 'R' };
#define ACNH_RING 32
typedef struct { uint64_t ns; int16_t core; char why; } acnh_core_ev;
static struct { uint64_t hle_top; _Atomic unsigned n; acnh_core_ev e[ACNH_RING]; } g_acnh_td[MAX_H];   /* by handle - 0x1000 */
static int acnh_stackcb_on(void) { static int on = -1; if (on < 0) on = getenv("ACNH_STACKCB") != NULL; return on; }
static void acnh_core_ev_add(const thr_t *t, int core, char why) {
    if (!acnh_stackcb_on() || !t) return;
    unsigned i = t->handle - 0x1000; if (i == 0 || i >= MAX_H) return;
    unsigned k = atomic_fetch_add(&g_acnh_td[i].n, 1) % ACNH_RING;
    g_acnh_td[i].e[k] = (acnh_core_ev){now_ns(), (int16_t)core, why};
}
/* thread_main: the host-stack top this thread's host services run below (hle_stack.S keeps the guest SP 16 bytes under it) */
static void acnh_note_hle_top(const thr_t *t, uintptr_t top) {
    if (!acnh_stackcb_on() || !t) return;
    unsigned i = t->handle - 0x1000; if (i && i < MAX_H) g_acnh_td[i].hle_top = top & ~(uintptr_t)15;
}

/* The guest thread whose stack holds addr: its host (pthread) stack -- the main thread runs guest code there -- or its guest
   stack (the nearest stack_top above addr, within 16 MiB). Returns the handle, 0 when none. */
uint32_t acnh_stack_owner(uint64_t addr) {
    uint32_t best = 0; uint64_t best_d = 16ull << 20;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited) continue;
        uint64_t hi = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(t->pt), lo = hi - pthread_get_stacksize_np(t->pt);
        if (addr >= lo && addr < hi) { best = t->handle; break; }
        if (t->stack_top && addr < t->stack_top && t->stack_top - addr < best_d) { best_d = t->stack_top - addr; best = t->handle; }
    }
    pthread_mutex_unlock(&G);
    return best;
}
/* Where thread h is now: its PC, its guest SP and the virtual core it holds. The thread is suspended for the read (never the
   caller). In guest code the SP is the guest SP; in a host service it is the one hle_stack.S saved (the main thread's guest
   code runs on its host stack, so its SP is its guest SP either way). Returns 0 when h is no live thread. */
int acnh_thread_sample(uint32_t h, uint64_t *pc, uint64_t *gsp, int *core) {
    pthread_mutex_lock(&G);
    thr_t *t = h_peek(h, O_THREAD); mach_port_t port = t && !t->exited ? t->mport : MACH_PORT_NULL;
    int c = t ? t->cur_core : -1; uint64_t top = t ? g_acnh_td[h - 0x1000].hle_top : 0;
    uint64_t hhi = t ? (uint64_t)(uintptr_t)pthread_get_stackaddr_np(t->pt) : 0, hlo = t ? hhi - pthread_get_stacksize_np(t->pt) : 0;
    int self = t && pthread_equal(t->pt, pthread_self());
    pthread_mutex_unlock(&G);
    if (!port || self) return 0;
    arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_suspend(port) != KERN_SUCCESS) return 0;
    kern_return_t kr = thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
    uint64_t sp = arm_thread_state64_get_sp(st), saved = 0;
    if (kr == KERN_SUCCESS && top && sp >= hlo && sp < hhi && sp < top) saved = *(volatile uint64_t *)(uintptr_t)(top - 16);   /* in a host service */
    thread_resume(port);
    if (kr != KERN_SUCCESS) return 0;
    *pc = arm_thread_state64_get_pc(st); *gsp = saved ? saved : sp; *core = c;
    return 1;
}
/* Logs thread h's last core events, oldest first, ms before now. */
void acnh_thread_ring_dump(uint32_t h, const char *tag) {
    unsigned i = h - 0x1000; if (i == 0 || i >= MAX_H) return;
    unsigned n = atomic_load(&g_acnh_td[i].n), from = n > ACNH_RING ? n - ACNH_RING : 0; uint64_t now = now_ns();
    char line[ACNH_RING * 24 + 1]; size_t k = 0;
    for (unsigned j = from; j < n && k + 24 < sizeof line; j++) { const acnh_core_ev *e = &g_acnh_td[i].e[j % ACNH_RING];
        k += snprintf(line + k, sizeof line - k, " %c%d@-%.2f", e->why, e->core, (double)(now - e->ns) / 1e6); }
    LOG("[stackcb] %s thread %#x core events (T take, G granted, S granted after starving, P put, X preempted, R read core #; ms ago):%s", tag, h, k ? line : " none");
}
#define ACNH_CORE_EV(t, c, w) acnh_core_ev_add((t), (c), (w))
#define ACNH_NOTE_HLE_TOP(t, top) acnh_note_hle_top((t), (top))
#else
#define ACNH_CORE_EV(t, c, w) ((void)0)
#define ACNH_NOTE_HLE_TOP(t, top) ((void)0)
#endif
#endif
