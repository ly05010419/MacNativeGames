#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include "../third_party/d2r_gfx/gfx/nvn/nvn_window_serial_set.h"

static nvn_window_serial_set set = NVN_WINDOW_SERIAL_SET_INIT;

static void *add_range(void *arg) {
    uint64_t base = (uint64_t)(uintptr_t)arg;
    for (uint64_t i = base; i < base + 32; i++)
        assert(nvn_window_serial_add(&set, i) == 1);
    return NULL;
}

static void *read_published(void *arg) {
    (void)arg;
    int n;
    do {
        n = atomic_load_explicit(&set.count, memory_order_acquire);
        for (int i = 0; i < n; i++) {
            uint64_t serial = atomic_load_explicit(&set.serial[i], memory_order_relaxed);
            assert(serial && nvn_window_serial_contains(&set, serial));
        }
        sched_yield();
    } while (n < NVN_WINDOW_SERIAL_CAPACITY);
    return NULL;
}

int main(void) {
    assert(!nvn_window_serial_contains(&set, 1));
    assert(nvn_window_serial_add(&set, 0) == 0);
    pthread_t threads[4], reader;
    assert(!pthread_create(&reader, NULL, read_published, NULL));
    for (uintptr_t i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, add_range, (void *)(i * 32 + 1)));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_join(reader, NULL));
    assert(atomic_load(&set.count) == NVN_WINDOW_SERIAL_CAPACITY);
    for (uint64_t i = 1; i <= 128; i++) assert(nvn_window_serial_contains(&set, i));
    assert(nvn_window_serial_add(&set, 1) == 0);
    assert(nvn_window_serial_add(&set, 129) == -1);
    assert(!nvn_window_serial_contains(&set, 129));
    return 0;
}
