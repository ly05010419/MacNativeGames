/* Probe of games/totk/third_party/d2r_gfx/gfx/nvn/nvn_alias_plan.h (tests/test_alias_region_plan.py): the range of
   the region heap made for a render target no region holds. Prints one line per check and exits 1 on a failure. */
#include <stdint.h>
#include <stdio.h>
#include "nvn_alias_plan.h"

#define MIB (UINT64_C(1) << 20)
#define KIB (UINT64_C(1) << 10)
static int failed;
static void check(int ok, const char *what) { printf("%s: %s\n", ok ? "ok" : "FAILED", what); if (!ok) failed = 1; }

int main(void) {
    /* 1. which region holds a texture */
    { alias_span r[] = {{0, 50 * MIB, 2500, 7}, {60 * MIB, 18 * MIB, 3, 9}, {200 * MIB, 4 * MIB, 1, 8}};
      check(alias_region_find(r, 3, 10 * MIB, 12 * MIB, 128) == 0, "a texture inside a region goes there");
      check(alias_region_find(r, 3, 60 * MIB + 512, 61 * MIB, 128) == 1, "also in another region");
      check(alias_region_find(r, 3, 49 * MIB, 51 * MIB, 128) == -1, "a texture reaching past a region is in none");
      check(alias_region_find(r, 3, 60 * MIB + 100, 61 * MIB, 128) == -1, "nor one at an offset its alignment does not divide"); }
    /* 2. the measured case (2026-11-18, format 252): a 50 MiB region with 2,122 textures, a 2,192 KiB texture reaching
       191 KiB past its top: the union takes a quarter of its size as room above, the base stays */
    { alias_span r[] = {{0x15ae80200, 50272 * KIB, 2122, 664}};
      uint64_t lo = 0x15ae80200 + 50272 * KIB - 2001 * KIB, hi = lo + 2192 * KIB;
      alias_plan p = alias_region_plan(r, 1, NULL, 0, lo, hi, 1);
      check(p.merged == 1 && p.lo == 0x15ae80200 && p.hi == hi + 12608 * KIB, "a region with many textures grows with a quarter of its size as room above");
      p = alias_region_plan(r, 1, NULL, 0, lo, hi, 0);
      check(p.merged == 1 && p.lo == 0x15ae80200 && p.hi == hi, "with room off it is the exact union, as before"); }
    /* 3. few textures: the exact union */
    { alias_span r[] = {{8 * MIB, 4 * MIB, 3, 2}};
      alias_plan p = alias_region_plan(r, 1, NULL, 0, 11 * MIB, 14 * MIB, 1);
      check(p.merged == 1 && p.lo == 8 * MIB && p.hi == 14 * MIB, "a region with few textures gets the exact union");
      p = alias_region_plan(r, 1, NULL, 0, 6 * MIB, 9 * MIB, 1);
      check(p.merged == 1 && p.lo == 6 * MIB && p.hi == 12 * MIB, "also when the texture starts below it"); }
    /* 4. nothing touched: the texture's own range */
    { alias_span r[] = {{0, 50 * MIB, 2500, 7}};
      alias_plan p = alias_region_plan(r, 1, NULL, 0, 80 * MIB, 82 * MIB, 1);
      check(p.merged == 0 && p.lo == 80 * MIB && p.hi == 82 * MIB, "a texture alone gets a region of its own size");
      p = alias_region_plan(NULL, 0, NULL, 0, 80 * MIB, 82 * MIB, 1);
      check(p.merged == 0 && p.lo == 80 * MIB && p.hi == 82 * MIB, "also with no region at all"); }
    /* 5. growing downward (most of the growth of three of the four formats): room below, in 64 KiB steps from the base */
    { alias_span r[] = {{20 * MIB + 512, 50 * MIB, 400, 7}};
      uint64_t lo = 19 * MIB + 512, hi = 21 * MIB + 512;
      alias_plan p = alias_region_plan(r, 1, NULL, 0, lo, hi, 1);
      check(p.merged == 1 && p.hi == 70 * MIB + 512 && p.lo == lo - 13056 * KIB, "a texture starting below takes a quarter as room below, none above");
      check((lo - p.lo) % (64 * KIB) == 0, "and the new base keeps the alignment of every offset");
      alias_span low[] = {{6 * MIB, 3 * MIB, 400, 7}};
      p = alias_region_plan(low, 1, NULL, 0, 5 * MIB, 7 * MIB, 1);
      check(p.lo == 3 * MIB && p.hi == 9 * MIB, "small regions take at least 2 MiB");
      alias_span zero[] = {{1 * MIB, 3 * MIB, 400, 7}};
      p = alias_region_plan(zero, 1, NULL, 0, MIB / 2, 2 * MIB, 1);
      check(p.lo <= MIB / 2 && (MIB / 2 - p.lo) % (64 * KIB) == 0, "and the room does not wrap below address 0"); }
    /* 6. the room stops at the neighbouring regions of the format */
    { alias_span r[] = {{0, 50 * MIB, 2500, 7}, {55 * MIB, 10 * MIB, 1, 3}};
      alias_plan p = alias_region_plan(r, 2, NULL, 0, 49 * MIB, 51 * MIB, 1);
      check(p.merged == 1 && p.lo == 0 && p.hi == 55 * MIB, "the room above ends where the next region begins");
      alias_span q[] = {{20 * MIB, 50 * MIB, 400, 7}, {5 * MIB, 13 * MIB + 512 * KIB, 1, 3}};
      p = alias_region_plan(q, 2, NULL, 0, 19 * MIB, 21 * MIB, 1);
      check(p.merged == 1 && p.lo == 18 * MIB + 512 * KIB && p.hi == 70 * MIB, "the room below ends where the region under it ends"); }
    /* 7. the formats share the stretch: grow as far as another format's big region over these addresses already goes */
    { alias_span r[] = {{100 * MIB, 33 * MIB, 600, 7}};
      alias_span others[] = {{98 * MIB, 72 * MIB, 400, 5}, {300 * MIB, 10 * MIB, 500, 6}, {90 * MIB, 90 * MIB, 3, 4}};
      alias_plan p = alias_region_plan(r, 1, others, 3, 132 * MIB, 134 * MIB + 192 * KIB, 1);
      check(p.merged == 1 && p.lo == 100 * MIB && p.hi == 170 * MIB, "growing up it reaches the top of the other format's region here");
      p = alias_region_plan(r, 1, others, 3, 99 * MIB + 512 * KIB, 101 * MIB, 1);
      check(p.merged == 1 && p.hi == 133 * MIB && p.lo == 99 * MIB + 512 * KIB - 8576 * KIB, "growing down it takes its own room when that reaches lower than the other format's");
      alias_span deep[] = {{80 * MIB, 90 * MIB, 400, 5}};
      p = alias_region_plan(r, 1, deep, 1, 99 * MIB + 512 * KIB, 101 * MIB, 1);
      check(p.merged == 1 && p.hi == 133 * MIB && p.lo == 99 * MIB + 512 * KIB - (19 * MIB + 512 * KIB) / (64 * KIB) * (64 * KIB), "or the other format's bottom when that is lower"); }
    /* 8. the measured growth: 85 steps of 430 KiB at the top of a 33 MiB region with 600 textures (33 -> 69 MiB in 55 s).
       Before: 85 replacements of the whole region. Now: a few, and the heap is at most a quarter larger than needed. */
    { for (int room = 0; room < 2; room++) {
          alias_span r[1] = {{0x15ae80200, 33 * MIB, 600, 1}}; int replaced = 0; uint64_t top = r[0].lo + r[0].size;
          for (int step = 0; step < 85; step++) {
              uint64_t lo = top + 430 * KIB - 2192 * KIB, hi = lo + 2192 * KIB; top = hi;
              if (alias_region_find(r, 1, lo, hi, 128) >= 0) continue;
              alias_plan p = alias_region_plan(r, 1, NULL, 0, lo, hi, room);
              r[0] = (alias_span){p.lo, p.hi - p.lo, 600, (uint64_t)(replaced + 2)}; replaced++;
          }
          printf("   36 MiB of growth, room %s: the region was replaced %d times, heap %.1f MiB for an extent of %.1f MiB\n", room ? "on" : "off", replaced,
                 r[0].size / 1048576.0, (top - r[0].lo) / 1048576.0);
          if (!room) check(replaced == 85, "without room every step replaces the region");
          else { check(replaced >= 1 && replaced <= 4, "with room 36 MiB of growth replaces it at most 4 times");
                 check(r[0].size <= (top - r[0].lo) + (top - r[0].lo) / 4, "and the heap is at most a quarter larger than the extent"); } } }
    return failed;
}
