#pragma once
#include "nvn_refl.h"

/* Immutable shader metadata only. Slots 0..127 are texture handles and 128..159
   are image handles. Preserve order and duplicates: resolving a texture may
   upload it or request an encoder split. Never cache the handle or its view. */
static inline unsigned nvn_texture_plan(const nvn_refl *rf, uint16_t slots[NVN_REFL_MAXB]) {
    unsigned n = 0;
    for (int i = 0; i < rf->nb; i++) {
        const nvn_rbind *b = &rf->b[i];
        if ((b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) && b->reg < 128)
            slots[n++] = b->reg;
        else if (b->kind == NVN_RB_IMG && b->reg < 32)
            slots[n++] = 128 + b->reg;
    }
    return n;
}
