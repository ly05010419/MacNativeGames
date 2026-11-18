// Creating and destroying a guest thread must not leak host heap (item 1 of the memory-leak work): guest_thread_init allocates a TLS block and a thread-local region for each thread,
// and previously the keys were created with a null destructor and nothing was freed at thread exit, leaking 8192 + 0x200 + 0x400 = 9728 bytes per thread.
// Method: include the real thread.c directly and replace its calloc / free with the counting versions here; the TLS / thread-local-region keys are created the way the loader does (null destructor), and freeing relies on the extra key thread.c creates itself.
// Expected values are hard-coded: create and destroy 200 threads, bytes allocated = 200 x (9728 + 16 for recording the pointers) = 1948800, blocks still not freed after all threads exit = 0.
// Before exiting, each thread overwrites the slots of both keys with other pointers (which is what the guest does: it sets its own thread pointer) - a destructor that frees the value in the slot would crash (the first version of the fix crashed right here on the real host).
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <malloc/malloc.h>
static _Atomic long g_live_blocks, g_live_bytes, g_total_bytes;
static void *t_calloc(size_t n, size_t sz) { void *p = calloc(n, sz); atomic_fetch_add(&g_live_blocks, 1); atomic_fetch_add(&g_live_bytes, (long)(n * sz)); atomic_fetch_add(&g_total_bytes, (long)(n * sz)); return p; }
static _Atomic long g_freed_req;   // number of frees
static void t_free(void *p) { if (!p) return; atomic_fetch_sub(&g_live_blocks, 1); atomic_fetch_add(&g_freed_req, 1); free(p); }
#define calloc t_calloc
#define free t_free
#include "../thread.c"
#undef calloc
#undef free

pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
uint64_t dm_gate_top[1024]; const uint8_t dm_gate_stubs[16 * 1024];   // entry.S's gate tables (guest_gate_stack, which this test does not call)

static int fails, checks;
#define EXPECT(name, want, got) do { long w_ = (long)(want), g_ = (long)(got); checks++; printf("%-46s expected %-8ld → got %-8ld %s\n", name, w_, g_, w_ == g_ ? "✓" : "✗"); if (w_ != g_) fails++; } while (0)
static char g_not_heap[64];
static void *one(void *a) { (void)a; guest_thread_init(); pthread_setspecific(g_keytls, g_not_heap); pthread_setspecific(g_keytlr, g_not_heap + 32); return NULL; }
int main(void) {
    enum { N = 200 };
    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) || pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) { printf("key creation failed\n"); return 2; }
    for (int i = 0; i < N; i++) { pthread_t t; if (pthread_create(&t, NULL, one, NULL)) { printf("thread creation failed\n"); return 2; } pthread_join(t, NULL); }
    EXPECT("bytes requested by 200 threads (200 × 9744)", 1948800, atomic_load(&g_total_bytes));
    EXPECT("blocks still not freed after all exited", 0, atomic_load(&g_live_blocks));
    EXPECT("free calls (3 blocks per thread)", 600, atomic_load(&g_freed_req));
    printf("t_thread_leak: %d checks, %d failed\n", checks, fails); return fails != 0;
}
