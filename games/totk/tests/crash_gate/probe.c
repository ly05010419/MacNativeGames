/* CPU only: the crash report gate (runtime/crash_gate.h, used by loader.c's fault and abort handlers; after nxnative
   d47f899). The probe runs itself in child processes, one per case, and reads their stderr and exit status. The child's
   handler does what loader.c's crash() does: a fault under a sigsetjmp guard (runtime_fault_guard) jumps back first,
   then crash_gate, then a slow "dump" (BEGIN, 300 ms, END) and _exit(128 + signal).
     many    8 threads fault at once. Expected: one BEGIN / END, exit 139 (SIGSEGV), well under the 15 s limit.
     stall   the dump never ends. Expected: the 1 s alarm ends the process with "crash report stalled", exit 139.
     reenter the printing thread traps (SIGTRAP) inside its own dump. Expected: it exits at once, 128 + SIGTRAP = 133,
             without END.
     guard   while one thread prints its crash, another thread takes 1,000 guarded faults. Expected: all 1,000 jump back
             (GUARD 1000 before END): a guarded fault is never stopped by the gate. Exit 139.
   Build from games/totk (outside the project for the output):
     xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror tests/crash_gate/probe.c -o /tmp/crash-gate-probe
   Negative control: -DCRASH_GATE_OFF builds the same handler without the gate: `many` prints 8 BEGINs, `stall` and
   `reenter` never end by themselves (the parent kills them after 5 s); `guard` passes either way, as the guard is
   handled before the gate. */
#define _DARWIN_C_SOURCE
#include "../../runtime/crash_gate.h"
#include <mach/mach.h>
#include <setjmp.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
extern char **environ;

static const char *g_mode;
static __thread sigjmp_buf *g_guard;
static volatile int g_dumping;
static void say(const char *s) { ssize_t w = write(2, s, strlen(s)); (void)w; }
static void handler(int sig, siginfo_t *si, void *uc) {
    (void)si; (void)uc;
    if ((sig == SIGSEGV || sig == SIGBUS) && g_guard) { sigjmp_buf *g = g_guard; g_guard = NULL; siglongjmp(*g, 1); }
#ifndef CRASH_GATE_OFF
    crash_gate(sig, strcmp(g_mode, "stall") ? 15 : 1);
#else
    (void)crash_gate;
#endif
    say("BEGIN\n"); g_dumping = 1;
    if (!strcmp(g_mode, "stall")) for (;;) pause();
    if (!strcmp(g_mode, "reenter")) __builtin_trap();
    struct timespec d = {0, 300000000}; nanosleep(&d, NULL);
    say("END\n"); _exit(128 + sig);
}
static volatile int g_ready, g_go;   /* Darwin has no pthread barriers: a counter instead */
static void fault(void) { *(volatile int *)(uintptr_t)8 = 1; }
static void *crasher(void *unused) { (void)unused; __atomic_fetch_add(&g_ready, 1, __ATOMIC_SEQ_CST); while (!g_go) {} fault(); return NULL; }
static void *guarded(void *unused) {
    (void)unused;
    while (!g_dumping) {}
    volatile unsigned back = 0;   /* changed between sigsetjmp and siglongjmp */
    for (volatile unsigned i = 0; i < 1000; i++) {   /* the fences keep the guard stores around the fault, as the call to memcpy does in nvn_mtl.m */
        sigjmp_buf j; if (sigsetjmp(j, 1)) { back++; continue; }
        g_guard = &j; __atomic_signal_fence(__ATOMIC_SEQ_CST); fault(); __atomic_signal_fence(__ATOMIC_SEQ_CST); g_guard = NULL; }
    char b[32]; snprintf(b, sizeof b, "GUARD %u\n", (unsigned)back); say(b);
    return NULL;
}
static int child(const char *mode) {
    g_mode = mode;
    /* A task exception port inherited from the shell that started the probe (seen in agent shells: mask 0x184e) takes
       hardware faults before the kernel turns them into signals and never answers, so the child would hang at its first
       fault. The child drops it; the game is not affected by this probe either way. */
    task_set_exception_ports(mach_task_self(), EXC_MASK_ALL, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
    struct sigaction a = {0}; a.sa_sigaction = handler; a.sa_flags = SA_SIGINFO; sigemptyset(&a.sa_mask);
    sigaction(SIGSEGV, &a, NULL); sigaction(SIGBUS, &a, NULL); sigaction(SIGTRAP, &a, NULL);
    pthread_t t[8];
    if (!strcmp(mode, "many")) { for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, crasher, NULL); while (g_ready < 8) {} g_go = 1; for (;;) pause(); }
    if (!strcmp(mode, "guard")) { pthread_create(&t[0], NULL, guarded, NULL); fault(); }
    fault();
    return 1;
}

/* ---------- parent ---------- */
static int run(const char *self, const char *mode, char *out, size_t cap, double *secs) {
    int p[2]; if (pipe(p)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 2); posix_spawn_file_actions_addclose(&fa, p[0]);
    char *argv[] = {(char *)self, (char *)mode, NULL}; pid_t pid;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    if (posix_spawn(&pid, self, &fa, NULL, argv, environ)) return -1;
    close(p[1]);
    size_t n = 0; ssize_t k; int st = 0;
    for (;;) {   /* read until the child closes stderr; give up after 5 s */
        fd_set fs; FD_ZERO(&fs); FD_SET(p[0], &fs); struct timeval tv = {0, 100000};
        if (select(p[0] + 1, &fs, NULL, NULL, &tv) > 0) { if ((k = read(p[0], out + n, cap - 1 - n)) <= 0) break; n += (size_t)k; }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (t1.tv_sec - t0.tv_sec > 5) { kill(pid, SIGKILL); break; }
    }
    out[n] = 0; close(p[0]); waitpid(pid, &st, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1); *secs = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
}
static int count(const char *s, const char *w) { int n = 0; for (const char *p = s; (p = strstr(p, w)); p += strlen(w)) n++; return n; }
int main(int argc, char **argv) {
    if (argc > 1) return child(argv[1]);
    char out[4096]; double s; int fails = 0, code;
    code = run(argv[0], "many", out, sizeof out, &s);
    int ok = code == 139 && count(out, "BEGIN") == 1 && count(out, "END") == 1 && s < 5;
    printf("many: exit %d, %d BEGIN, %d END, %.2f s %s\n", code, count(out, "BEGIN"), count(out, "END"), s, ok ? "pass" : "FAIL"); fails += !ok;
    code = run(argv[0], "stall", out, sizeof out, &s);
    ok = code == 139 && strstr(out, "crash report stalled") && s > 0.9 && s < 3;
    printf("stall: exit %d, %.2f s, %s %s\n", code, s, strstr(out, "stalled") ? "stall line" : "no stall line", ok ? "pass" : "FAIL"); fails += !ok;
    code = run(argv[0], "reenter", out, sizeof out, &s);
    ok = code == 128 + SIGTRAP && count(out, "BEGIN") == 1 && !count(out, "END");
    printf("reenter: exit %d, %d BEGIN, %d END %s\n", code, count(out, "BEGIN"), count(out, "END"), ok ? "pass" : "FAIL"); fails += !ok;
    code = run(argv[0], "guard", out, sizeof out, &s);
    const char *g = strstr(out, "GUARD 1000"), *e = strstr(out, "END");
    ok = code == 139 && g && e && g < e;
    printf("guard: exit %d, %s %s\n", code, g ? "GUARD 1000" : "guarded faults lost", ok ? "pass" : "FAIL"); fails += !ok;
    printf("{\"status\":\"%s\",\"cases\":4,\"failed\":%d}\n", fails ? "fail" : "pass", fails);
    return fails != 0;
}
