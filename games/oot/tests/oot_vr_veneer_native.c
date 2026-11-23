/* Native arm64 test of runtime/oot_vr_veneer.h (driven by oot_vr_veneer_native.py). Built twice: with -DGEN it prints
   the veneers' words (the script lays them into assembly between our entry and return labels); without, it runs them. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/oot_vr_veneer.h"

enum { STUB = 0x10000, PAGE = 0x40000, FN_CELL = 80, FOV_CELL = 72 };

#ifdef GEN
int main(void) {
    uint32_t call[OOT_VR_CALL_WORDS], fov[OOT_VR_FOV_WORDS];
    /* The return label follows the veneer: the "site" is the word before it. */
    const char *e = oot_vr_call_veneer(0xf9401268u, STUB, STUB + 62 * 4, PAGE, FN_CELL, call);
    if (e) { fprintf(stderr, "%s\n", e); return 1; }
    e = oot_vr_fov_veneer(0xbd413e60u, STUB, STUB + 20, PAGE, FOV_CELL, fov);
    if (e) { fprintf(stderr, "%s\n", e); return 1; }
    printf("call");
    for (int i = 0; i < OOT_VR_CALL_WORDS; i++) printf(" %08x", call[i]);
    printf("\nfov");
    for (int i = 0; i < OOT_VR_FOV_WORDS; i++) printf(" %08x", fov[i]);
    printf("\n");
    /* Refusals: a site that is not the expected load, an unaligned cell. */
    if (!oot_vr_fov_veneer(0xf9401268u, STUB, STUB + 20, PAGE, FOV_CELL, fov)) return 2;
    if (!oot_vr_call_veneer(0xf9401268u, STUB, STUB + 62 * 4, PAGE, 81, call)) return 3;
    if (!oot_vr_call_veneer(0xf9401268u, STUB, STUB + 0x10000000, PAGE, FN_CELL, call)) return 4;
    return 0;
}
#else
extern unsigned char vr_page[4096];
extern uint64_t vr_out[128], vr_seen[2];
extern void run_call(const uint64_t in[20]);
extern float run_fov(const void *camera);
extern void probe(void);

int main(void) {
    _Alignas(16) unsigned char camera[0x200] = {0};
    const uint64_t field = 0xfeedfacecafebeefULL;
    memcpy(camera + 0x20, &field, 8);
    float angle = 0.785f;
    memcpy(camera + 0x13c, &angle, 4);

    void (*fn)(void) = probe;
    memcpy(vr_page + FN_CELL, &fn, 8);
    uint64_t in[20];
    for (int i = 0; i < 18; i++) in[i] = 0x1111111111111111ULL * (uint64_t)(i % 15 + 1) + (uint64_t)i;
    in[18] = (uint64_t)(uintptr_t)camera;              /* x19: the camera */
    in[19] = 0x2020202020202020ULL;                    /* x20: the view */
    run_call(in);
    assert(vr_seen[0] == in[19] && vr_seen[1] == in[18]);                     /* the host saw x0 = x20, x1 = x19 */
    for (int i = 0; i < 18; i++) assert(vr_out[i] == (i == 8 ? field : in[i]));   /* x8: the displaced ldr x8, [x19, #0x20] */
    assert(vr_out[18] == 0x3030303030303030ULL);                              /* x30 */
    assert(vr_out[19] == 0x60000000ULL);                                      /* the flags (Z, C) */
    const unsigned char *q = (const unsigned char *)&vr_out[20];              /* q0-q7, q16-q31: 24 x 16 bytes, after 0x40+i */
    for (int r = 0, k = 0; r < 32; r++) {
        if (r >= 8 && r < 16) continue;
        for (int b = 0; b < 16; b++) assert(q[16 * k + b] == (unsigned char)(0x40 + r));
        k++;
    }

    float none = 0, wide = 1.62f;
    memcpy(vr_page + FOV_CELL, &none, 4);
    assert(run_fov(camera) == angle);                  /* cell 0: the game's own angle */
    memcpy(vr_page + FOV_CELL, &wide, 4);
    assert(run_fov(camera) == wide);                   /* the headset's */
    memcpy(camera + 0x13c, &wide, 4);
    memcpy(vr_page + FOV_CELL, &none, 4);
    assert(run_fov(camera) == wide);                   /* the camera itself was never written */
    puts("native ARM64 OOT VR veneers: the camera call keeps x0-x17, x30, flags, q0-q7 and q16-q31, hands x20 / x19 to the "
         "host and runs the displaced load; the field of view takes the data page's angle only while it is set");
    return 0;
}
#endif
