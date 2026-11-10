// Forced BUSY-timeout harness for nvn_dirty.c: leaves page 0 in PG_BUSY with no other thread to clear it,
// then hits on_fault / prep_write / gpu_write_begin / nvn_dirty_protect so each call's busy_wait times out.
// Built by tests/test_nvn_dirty_busy_timeout.py with DIRTY_C naming the nvn_dirty.c under test;
// -DBUSY_WAIT_MS shortens the wait; -DITERS sets how many rounds (4 sites each).
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include DIRTY_C

void dm_fault_hook_register(int (*fn)(void *, int, void *)) { (void)fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { (void)fn; }

#ifndef ITERS
#define ITERS 10
#endif

int main(void) {
    size_t pg = (size_t)getpagesize(), size = pg * 2;
    void *page = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) return 2;
    nvn_dirty_add_range(page, size);
    range_t *r = find((uintptr_t)page);
    if (!r) return 2;
    atomic_store(&r->state[0], PG_BUSY);   // stuck BUSY: no peer thread clears it

    int n_fault = 0, n_prep = 0, n_gpu = 0, n_prot = 0;
    for (int i = 0; i < ITERS; i++) {
        on_fault(page, 1, NULL); n_fault++;
        prep_write(page, 16); n_prep++;
        nvn_dirty_gpu_write_end(nvn_dirty_gpu_write_begin(page, 16)); n_gpu++;
        nvn_dirty_protect(page, size); n_prot++;
    }
    printf("on_fault returned %d\n", n_fault);
    printf("prep_write returned %d\n", n_prep);
    printf("gpu_write_begin returned %d\n", n_gpu);
    printf("nvn_dirty_protect returned %d\n", n_prot);
    return 0;
}
