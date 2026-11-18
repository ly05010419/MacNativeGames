// Guest thread environment: gate slot = host gate entry; x18 slot cleared; tpidr_el0 slot = ELF TLS block; tpidrro_el0 slot = Horizon ThreadLocalRegion (0x200).
// The SDK's internal critical section takes the current thread object pointer from TLR[0x1f8]; here each thread gets a zero-initialized object allocated and written in.
#include "dm.h"
#define TLS_SIZE 8192
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400
extern void dm_gate(void);            // entry.S: common gate entry
static pthread_key_t g_keyown;   // raw pointers of the TLS block / thread-local region this thread allocated (see the end of guest_thread_init)
static pthread_once_t g_keyown_once = PTHREAD_ONCE_INIT;
static void own_free(void *p) { void **own = p; free(own[0]); free(own[1]); free(own); }
static void own_key_init(void) { if (pthread_key_create(&g_keyown, own_free)) die("cannot create the free key for thread-local blocks"); }
void guest_thread_init(void) {
    pthread_setspecific(g_keygate, (void *)dm_gate);
    pthread_setspecific(g_key18, 0);
    // The TLS block and the thread-local region are freed when the thread exits. The guest overwrites the slots of the TLS / thread-local-region keys (it sets its own thread pointer), so they cannot be freed by the current value in the key:
    // the raw pointers that were allocated are recorded separately on this file's own key g_keyown and freed by its destructor (key destructors run at pthread_exit, like crash.c's signal alternate stack; the destructor only calls free, which is safe on a guest thread's small stack too).
    // They used not to be freed: every short-lived thread the game started leaked 8192 + 0x200 + 0x400 = 9728 bytes. The thread-local region and the current thread object are one block (the SDK may also overwrite the TLR[0x1f8] slot)
    pthread_once(&g_keyown_once, own_key_init);
    void **own = calloc(2, sizeof *own); uint8_t *tlr = calloc(1, TLR_SIZE + THREAD_OBJ_SIZE);
    if (!own || !tlr) die("guest thread-local block allocation failed");
    own[0] = calloc(1, TLS_SIZE); own[1] = tlr;
    if (!own[0]) die("guest thread TLS block allocation failed");
    *(void **)(tlr + 0x1f8) = tlr + TLR_SIZE;               // current thread object
    pthread_setspecific(g_keytls, own[0]); pthread_setspecific(g_keytlr, tlr);
    void **prev = pthread_getspecific(g_keyown); if (prev) own_free(prev);   // re-initialised on the same thread: free the previous set first (the new one is what the slots now point to)
    pthread_setspecific(g_keyown, own);
}
