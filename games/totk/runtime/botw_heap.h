/* Breath of the Wild heap sizes (botw_heap.c): one plan for the 2K output's +224 MiB and the 8 GB mode, prepared
   (every word read, the budget checked, the veneer bytes reserved) before any game patch writes, applied after. One
   table of sites per game version, one planner. No runtime dependency: the tests build it alone
   (tests/test_botw_heap.py). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define BOTW_HEAP_MAX_PUT 24
typedef struct { uint32_t off, word; } BotwHeapWord;
typedef struct {
    uint64_t heap, root;                 /* SetMemoryHeapSize and the root block, bytes */
    uint64_t veneer;                     /* main offset of the heap veneer (the root one is +0x10); 0 without veneers */
    unsigned n;                          /* words to write; 0 = nothing (the shipped sizes) */
    BotwHeapWord put[BOTW_HEAP_MAX_PUT];
} BotwHeapPlan;

/* The checked words: off, the version's word there, and for a written site its word in the 8 GB mode (0: a neighbour,
   read only). */
typedef struct { uint32_t off, original, big; } BotwHeapSite;
/* One version's sites. The first six entries are the heap site and the root site, each between its two neighbours;
   heap_rd / root_rd: the register the size goes to there (the veneers load it with movz + movk, the 2K output with
   mov w). */
typedef struct {
    const char *version;
    const BotwHeapSite *sites;
    unsigned count;
    uint32_t heap_at, heap_back, root_at, root_back;
    unsigned heap_rd, root_rd;
} BotwHeapTable;
extern const BotwHeapTable botw_heap_160, botw_heap_190;

/* 0 and a plan, or -1 and a reason in err (nothing written either way). size: the module's mapped bytes; text_end: the
   end of its code (the veneers go in the padding after it, behind the 16 bytes kept for the sound-link veneer). pool,
   heap_cap and image are what SetHeapSize checks against (services.c). */
int botw_heap_prepare(const BotwHeapTable *t, const uint8_t *base, uint64_t size, uint64_t text_end, int output_2k,
                      int dram_8gb, uint64_t pool, uint64_t heap_cap, uint64_t image, BotwHeapPlan *plan, char *err,
                      size_t errlen);
void botw_heap_apply(uint8_t *base, const BotwHeapPlan *plan);
