// Unit test (crash.c alternate stack is freed with the thread): start 2000 short-lived threads each calling crash_thread_init, and expect the heap not to grow with the thread count (2 calls per thread, 64 KiB each; before the fix a 250 MiB leak was measured)
#include <pthread.h>
#include <stdio.h>
#include <malloc/malloc.h>
void crash_thread_init(void);
static void *body(void *a) { crash_thread_init(); crash_thread_init(); return a; }
#if defined(__has_feature) && __has_feature(address_sanitizer)
#include <sanitizer/allocator_interface.h>
// ASan's malloc zone statistics do not go down on free (make asan): its allocator counts the live bytes instead
static size_t inuse(void) { return __sanitizer_get_current_allocated_bytes(); }
#else
static size_t inuse(void) { malloc_statistics_t st; malloc_zone_statistics(NULL, &st); return st.size_in_use; }
#endif
int main(void) {
    size_t before = inuse();
    for (int i = 0; i < 2000; i++) { pthread_t t; pthread_create(&t, 0, body, 0); pthread_join(t, 0); }
    double grew = (inuse() - (double)before) / 1048576.0;
    printf("heap growth after 2000 short-lived threads %.2f MiB (expected < 1; measured 250 before the fix) %s\n", grew, grew < 1 ? "✓" : "✗");
    return !(grew < 1);
}
