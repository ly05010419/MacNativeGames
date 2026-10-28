/* Animal Crossing: New Horizons 1.2.1 -- Guards against null LocationAdjuster dereferences
   in nn::ui2d layout calculation/rendering (main+0x1c6e5b0). */

#include "acnh_ui_patch.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIT4_OFF 0x1c6e644ull
#define BIT5_OFF 0x1c6e68cull
#define LOC3_OFF 0x1c6e940ull

#define FALLBACK_OFF         0x1c6e608ull
#define RESUME_BIT4_OFF      0x1c6e65cull
#define RESUME_BIT5_OFF      0x1c6e6a4ull
#define RESUME_LOC3_OFF      0x1c6e950ull
#define RESUME_LOC3_NULL_OFF 0x1c6e964ull

uint64_t acnh_ui_resume_bit4;
uint64_t acnh_ui_resume_bit5;
uint64_t acnh_ui_resume_loc3;
uint64_t acnh_ui_resume_loc3_null;
uint64_t acnh_ui_fallback;

void acnh_ui_bit4_hook(void);
void acnh_ui_bit5_hook(void);
void acnh_ui_loc3_hook(void);

static _Atomic uint64_t g_acnh_ui_null_hits;

void acnh_ui_log_null_hit(uint32_t loc) {
    uint64_t hit = atomic_fetch_add(&g_acnh_ui_null_hits, 1) + 1;
    if (hit <= 10 || (hit <= 100 && (hit % 20) == 0) || (hit % 1000) == 0) {
        fprintf(stderr, "[acnh] [ui] LocationAdjuster NULL guard hit #%llu at loc %u (pane null crash avoided)\n",
                (unsigned long long)hit, (unsigned)loc);
    }
}

void acnh_ui_patch_install(Module *m) {
    const char *e = getenv("ACNH_UI_PATCH");
    if (e && !strcmp(e, "0")) {
        fprintf(stderr, "[acnh] ACNH_UI_PATCH=0: UI LocationAdjuster NULL guards not installed\n");
        return;
    }

    static const uint32_t want_bit4[4] = {0xf9400269, 0x2d430520, 0x2d420d22, 0xf94062c9};
    static const uint32_t want_bit5[4] = {0xf9400269, 0x2d430520, 0x2d420d22, 0x1e223800};
    static const uint32_t want_loc3[4] = {0xf94062c8, 0x1e291821, 0x1e202860, 0x2d450d02};

    uint32_t have[4];
    memcpy(have, m->base + BIT4_OFF, sizeof have);
    if (memcmp(have, want_bit4, sizeof have)) {
        fprintf(stderr, "[acnh] ACNH_UI_PATCH: main+%#llx mismatch (%08x %08x %08x %08x), patch aborted\n",
                BIT4_OFF, have[0], have[1], have[2], have[3]);
        return;
    }
    memcpy(have, m->base + BIT5_OFF, sizeof have);
    if (memcmp(have, want_bit5, sizeof have)) {
        fprintf(stderr, "[acnh] ACNH_UI_PATCH: main+%#llx mismatch (%08x %08x %08x %08x), patch aborted\n",
                BIT5_OFF, have[0], have[1], have[2], have[3]);
        return;
    }
    memcpy(have, m->base + LOC3_OFF, sizeof have);
    if (memcmp(have, want_loc3, sizeof have)) {
        fprintf(stderr, "[acnh] ACNH_UI_PATCH: main+%#llx mismatch (%08x %08x %08x %08x), patch aborted\n",
                LOC3_OFF, have[0], have[1], have[2], have[3]);
        return;
    }

    acnh_ui_fallback = (uint64_t)(uintptr_t)(m->base + FALLBACK_OFF);
    acnh_ui_resume_bit4 = (uint64_t)(uintptr_t)(m->base + RESUME_BIT4_OFF);
    acnh_ui_resume_bit5 = (uint64_t)(uintptr_t)(m->base + RESUME_BIT5_OFF);
    acnh_ui_resume_loc3 = (uint64_t)(uintptr_t)(m->base + RESUME_LOC3_OFF);
    acnh_ui_resume_loc3_null = (uint64_t)(uintptr_t)(m->base + RESUME_LOC3_NULL_OFF);

    // Install branch to acnh_ui_bit4_hook
    uint64_t t_bit4 = (uint64_t)(uintptr_t)&acnh_ui_bit4_hook;
    uint32_t w_bit4[4] = {0x58000050u /* ldr x16, #8 */, 0xd61f0200u /* br x16 */, (uint32_t)t_bit4, (uint32_t)(t_bit4 >> 32)};
    memcpy(m->base + BIT4_OFF, w_bit4, sizeof w_bit4);

    // Install branch to acnh_ui_bit5_hook
    uint64_t t_bit5 = (uint64_t)(uintptr_t)&acnh_ui_bit5_hook;
    uint32_t w_bit5[4] = {0x58000050u /* ldr x16, #8 */, 0xd61f0200u /* br x16 */, (uint32_t)t_bit5, (uint32_t)(t_bit5 >> 32)};
    memcpy(m->base + BIT5_OFF, w_bit5, sizeof w_bit5);

    // Install branch to acnh_ui_loc3_hook
    uint64_t t_loc3 = (uint64_t)(uintptr_t)&acnh_ui_loc3_hook;
    uint32_t w_loc3[4] = {0x58000050u /* ldr x16, #8 */, 0xd61f0200u /* br x16 */, (uint32_t)t_loc3, (uint32_t)(t_loc3 >> 32)};
    memcpy(m->base + LOC3_OFF, w_loc3, sizeof w_loc3);

    fprintf(stderr, "[acnh] ACNH_UI_PATCH: installed LocationAdjuster NULL guards at main+%#llx, main+%#llx, main+%#llx\n",
            BIT4_OFF, BIT5_OFF, LOC3_OFF);
}
