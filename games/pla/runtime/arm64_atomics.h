#pragma once
#include <stdint.h>
#include <stddef.h>

/* Lower a compiler-emitted exchange with a stack spill inside its exclusive
   window to an ARMv8.1 LSE exchange. A store in the reservation granule can
   otherwise invalidate STLXR on every retry on Apple silicon. No game address,
   stack distance or spare register is assumed. The spill and the successful
   store's zero status are preserved, as are acquire/release ordering and NZCV. */
static inline int arm64_exchange_words(const uint32_t in[4], uint32_t out[4]) {
    unsigned value = in[0] & 31, base = (in[0] >> 5) & 31;
    unsigned status = (in[2] >> 16) & 31;
    if ((in[0] & 0xfffffc00u) != 0xc85ffc00u || value == 31 || base == value ||
        base == 31 || status == 31 || status == base ||
        (in[1] & 0xffc003ffu) != (0xf90003e0u | value) ||
        (in[2] & 0xffe0ffffu) != (0xc800fc1fu | (base << 5)) ||
        in[3] != (0x35ffffa0u | status)) return 0;
    out[0] = 0xf8ff8000u | (base << 5) | value; /* swpal xzr,xVALUE,[xBASE] */
    out[1] = in[1];                            /* str xVALUE,[sp,#imm] */
    out[2] = 0x2a1f03e0u | status;             /* mov wSTATUS,wzr */
    out[3] = 0xd503201fu;
    return 1;
}

/* Mark direct branch destinations before changing a multi-instruction block.
   Reject alternate entries into its middle. The backward retry enters word 0. */
static inline int64_t arm64_direct_target(uint32_t word, size_t pc) {
    uint32_t bits; unsigned width;
    if ((word & 0x7c000000u) == 0x14000000u) { bits = word & 0x03ffffffu; width = 26; }
    else if ((word & 0x7e000000u) == 0x34000000u || (word & 0xff000010u) == 0x54000000u) { bits = (word >> 5) & 0x7ffffu; width = 19; }
    else if ((word & 0x7e000000u) == 0x36000000u) { bits = (word >> 5) & 0x3fffu; width = 14; }
    else return -1;
    int64_t displacement = (int64_t)(bits ^ (1u << (width - 1))) - (1ll << (width - 1));
    return (int64_t)pc + displacement;
}

static inline size_t arm64_lower_exchanges(uint32_t *text, size_t count, uint8_t *targets) {
    for (size_t i = 0; i < count; i++) targets[i] = 0;
    for (size_t i = 0; i < count; i++) {
        int64_t t = arm64_direct_target(text[i], i);
        if (t >= 0 && (uint64_t)t < count) targets[t] = 1;
    }
    size_t changed = 0;
    for (size_t i = 0; i + 4 <= count; i++) {
        uint32_t replacement[4];
        if (!targets[i+1] && !targets[i+2] && !targets[i+3] && arm64_exchange_words(text+i, replacement)) {
            for (size_t j=0; j<4; j++) text[i+j] = replacement[j];
            changed++; i += 3;
        }
    }
    return changed;
}
