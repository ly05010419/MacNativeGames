/* BotW's startup/root heaps are 64-bit arguments, originally loaded with one
   32-bit MOV. Above 4 GiB, use a guarded veneer in unused RX text padding.
   +0x100 leaves the sound, FOV and first-blend veneers untouched. */
#pragma once
#include <stdint.h>
#include <string.h>

static inline uint32_t botw_heap_branch(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)to - (int64_t)from;
    if ((d & 3) || d < -(1 << 27) || d >= (1 << 27)) return 0;
    return 0x14000000u | ((uint32_t)(d >> 2) & 0x3ffffffu);
}

static inline int botw_heap_patch(uint8_t *main, uint64_t size, uint64_t text_end,
                                  uint32_t site, uint32_t expected, unsigned reg,
                                  unsigned slot, uint64_t bytes) {
    uint64_t stub = ((text_end + 15) & ~15ULL) + 0x100 + slot * 16;
    uint64_t rx_end = (text_end + 0xfff) & ~0xfffULL;
    if (reg > 30 || slot > 1 || bytes <= UINT32_MAX || bytes >> 48 || (bytes & 0xffff) ||
        site + 4ULL > text_end || stub + 12 > rx_end || stub + 12 > size) return 0;
    uint32_t original;
    memcpy(&original, main + site, 4);
    if (original != expected) return 0;
    for (unsigned i = 0; i < 12; i++) if (main[stub + i]) return 0;
    uint32_t jump = botw_heap_branch(site, stub);
    uint32_t words[] = {
        0xd2a00000u | ((uint32_t)(bytes >> 16) & 0xffffu) << 5 | reg, /* movz xN, #low, lsl #16 */
        0xf2c00000u | ((uint32_t)(bytes >> 32) & 0xffffu) << 5 | reg, /* movk xN, #high, lsl #32 */
        botw_heap_branch(stub + 8, site + 4),
    };
    if (!jump || !words[2]) return 0;
    memcpy(main + stub, words, sizeof words);
    memcpy(main + site, &jump, 4);
    return 1;
}
