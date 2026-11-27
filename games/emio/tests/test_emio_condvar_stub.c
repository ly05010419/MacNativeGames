// Stubs for the Emio t_condvar port: host facilities of threads.c the unit test does not use.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "expected rejection: %s\n", message); exit(86); }
__thread void *runtime_fault_guard;
uint64_t runtime_now_ns(void) { return 0; }
void *runtime_symbol(const char *name) { (void)name; return NULL; }
void runtime_thread_init(void) {}
void runtime_thread_leave(void) {}
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) { (void)entry; (void)arg; (void)sp; abort(); }
