#include "../nvn/nvn_dirty.h"
#include "../../host/dm.h"
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-52s expected %-4lld → got %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static void *page_map(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}

int main(void) {
    crash_install();

    for (int i = 0; i < 65; i++) {
        void *p = page_map();
        nvn_dirty_add_range(p, 4096);
        nvn_dirty_remove_range(p);
        munmap(p, 4096);
    }

    void *reused = page_map();
    nvn_dirty_add_range(reused, 4096);
    nvn_dirty_protect(reused, 4096);
    uint64_t stamp = nvn_dirty_now();
    ((volatile uint8_t *)reused)[0] = 1;
    EXPECT("after 65 pool destroys in total, a write to a new pool is dirty", 1, nvn_dirty_since(reused, 4096, stamp));
    nvn_dirty_remove_range(reused);
    munmap(reused, 4096);

    void *live[300];
    for (int i = 0; i < 300; i++) {
        live[i] = page_map();
        nvn_dirty_add_range(live[i], 4096);
    }
    void *overflow = live[299];
    nvn_dirty_protect(overflow, 4096);
    stamp = nvn_dirty_now();
    ((volatile uint8_t *)overflow)[0] = 2;
    EXPECT("pools beyond the live limit are conservatively dirty", 1, nvn_dirty_since(overflow, 4096, stamp));
    for (int i = 0; i < 300; i++) {
        nvn_dirty_remove_range(live[i]);
        munmap(live[i], 4096);
    }

    // More pools alive at once than the table's fixed 4,096 slots used to hold (Breath of the Wild reaches 4,006 in its first shrine): the table grows, and the last pool is still tracked
    enum { NMANY = 5000 };
    void **many = malloc(NMANY * sizeof *many);
    for (int i = 0; i < NMANY; i++) { many[i] = page_map(); nvn_dirty_add_range(many[i], 4096); }
    void *last = many[NMANY - 1];
    EXPECT("5000 live pools: the last one is tracked", 1, nvn_dirty_tracked(last, 4096));
    nvn_dirty_protect(last, 4096);
    stamp = nvn_dirty_now();
    EXPECT("pool 5000 not written after protect: clean", 0, nvn_dirty_since(last, 4096, stamp));
    ((volatile uint8_t *)last)[0] = 5;
    EXPECT("pool 5000 written by the game: dirty", 1, nvn_dirty_since(last, 4096, stamp));
    for (int i = 0; i < NMANY; i++) { nvn_dirty_remove_range(many[i]); munmap(many[i], 4096); }
    free(many);

    // Adjacent pools sharing a host page (pools are 4 KB aligned, host pages 16 KB): the first page of SMBW's ASTC texture pool is shared with a small preceding pool, and was judged untracked and re-uploaded on every use
    size_t pg = (size_t)getpagesize();
    uint8_t *blk = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (blk == MAP_FAILED) { perror("mmap"); exit(2); }
    uint8_t *pa = blk, *pb = blk + pg - 0x1000;           // pool A covers the start of page 0, pool B starts in the last 4 KB of page 0
    size_t na = pg - 0x1000, nb = 2 * pg, tex = pg + 0x800;   // the texture in B spans the shared page and the next one
    nvn_dirty_add_range(pa, na);
    nvn_dirty_add_range(pb, nb);
    EXPECT("texture in the later of two page-sharing pools is tracked", 1, nvn_dirty_tracked(pb, tex));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    EXPECT("page-sharing texture not written after protect: clean", 0, nvn_dirty_since(pb, tex, stamp));
    pb[0x10] = 3;
    EXPECT("game writes B's part of the shared page: B's texture dirty", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    pa[0x10] = 4;   // same physical page, A's part: the page is read-only, the fault must be claimed through B's range or it is treated as a crash
    EXPECT("game writes A's part of the shared page: B's texture conservatively dirty", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    dm_guest_write_prepare(pa, 0x20); pa[0x20] = 5;   // the host writes A on behalf of the GPU: the shared page must also release B's protection
    EXPECT("host unprotects the shared page before writing: B's texture dirty", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_remove_range(pb);
    EXPECT("A still tracked after destroying B (ranges keyed by pool start)", 1, nvn_dirty_tracked(pa, na));
    EXPECT("B no longer tracked after destroying B", 0, nvn_dirty_tracked(pb, tex));
    nvn_dirty_remove_range(pa);
    munmap(blk, 4 * pg);
    return g_fail != 0;
}
