/* 合成的 CPU 回归：不加载游戏、shader 或 Metal。 | Synthetic CPU regression: no game, shaders or Metal. */
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_obj.h"
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/botw_ui_scissor_math.h"
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/botw_ui_scissor_origin.h"

#if defined(GAME_BOTW) && !defined(GAME_BOTW190)
_Static_assert(sizeof(p_scissor) == 32 && offsetof(p_scissor, ui_basis) == 16, "BOTW 1.6.0 scissor ABI");
#else
_Static_assert(sizeof(p_scissor) == 16, "Other versions retain their scissor ABI");
#endif

static void projection(float m[28]) {
    memset(m, 0, 28 * sizeof *m);
    m[0] = 0.125f; m[5] = 0.125f; m[14] = -1; m[26] = 1; m[27] = -2;
}
static void geometry(void) {
    float m[28], basis[4] = {64, 64, 128, 128};
    int32_t out[4], input[4] = {70, 68, 8, 12};
    projection(m);
    assert(botw_ui_scissor_map_rect(input, basis, m, out) == 1);
    assert(out[0] == 76 && out[1] == 72 && out[2] == 16 && out[3] == 24);
    int32_t neighbor[4] = {80, 80, 4, 8};
    assert(botw_ui_scissor_map_rect(neighbor, basis, m, out) == 1);
    assert(out[0] == 96 && out[1] == 96 && out[2] == 8 && out[3] == 16);
    int32_t edge[4] = {20, 20, 60, 50};
    assert(botw_ui_scissor_map_rect(edge, basis, m, out) == 1);
    assert(out[0] == 0 && out[1] == 0 && out[2] == 96 && out[3] == 76);
    m[0] = m[5] = 0.15625f;
    int32_t fractional[4] = {65, 65, 1, 1};
    assert(botw_ui_scissor_map_rect(fractional, basis, m, out) == 1);
    assert(out[0] == 66 && out[1] == 66 && out[2] == 3 && out[3] == 3);
    projection(m);
    int32_t outside[4] = {300, 300, 10, 10};
    assert(botw_ui_scissor_map_rect(outside, basis, m, out) == -1);
    float original = basis[0];
    const float bad[] = {0, -1, INFINITY, NAN, 20000};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        basis[0] = bad[i]; assert(botw_ui_scissor_map_rect(input, basis, m, out) == 0);
    }
    basis[0] = original;
    m[3] = 1; assert(botw_ui_scissor_map_rect(input, basis, m, out) == 0);
    m[3] = 0; m[27] = 0; assert(botw_ui_scissor_map_rect(input, basis, m, out) == 0);
    projection(m); m[0] = NAN; assert(botw_ui_scissor_map_rect(input, basis, m, out) == 0);
    projection(m); m[0] = 0.0625f; m[5] = 0.0625f;
    assert(botw_ui_scissor_map_rect(input, basis, m, out) == 0);
    projection(m); input[2] = 0;
    assert(botw_ui_scissor_map_rect(input, basis, m, out) == 0);
}

#if defined(GAME_BOTW) && !defined(GAME_BOTW190)
__thread sigjmp_buf *runtime_fault_guard;
static uint8_t *module;
static const size_t module_size = 0x1366000;
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off) {
    uintptr_t base = (uintptr_t)module;
    if (pc < base || pc - base >= module_size) return 0;
    *name = "main"; *off = pc - base; return 1;
}
static void fault(int sig) {
    (void)sig;
    assert(runtime_fault_guard);
    siglongjmp(*runtime_fault_guard, 1);
}
static void origin(void) {
    /* 空白合成模块中只放 ABI guard 常量，不含可执行游戏数据。 | Blank synthetic module, ABI guards only; no executable game data. */
    static const uint32_t guards[][2] = {
        {0x1273570,0xf81d0ff5},{0x1273574,0xa9014ff4},
        {0x1273578,0xa9027bfd},{0x127357c,0x910083fd},
        {0x1273720,0xd63f0100},{0x1365b30,0xd10243ff},
        {0x1365b3c,0xa9087bfd},{0x1365b40,0x910203fd},
        {0x1365e04,0x97fc35db}};
    module = calloc(1, module_size); assert(module);
    for (unsigned i = 0; i < sizeof guards / sizeof *guards; i++)
        memcpy(module + guards[i][0], &guards[i][1], 4);
    uint64_t first[2], second[2], native[2];
    _Alignas(16) uint8_t context[256] = {0};
    float framebuffer[8] = {0, 0, 100, 80, 0, 0, 200, 160};
    float viewport[8] = {0, 0, 0, 0, 100, 80, 0, 0};
    uintptr_t fb = (uintptr_t)framebuffer, vp = (uintptr_t)viewport;
    uintptr_t parent = (uintptr_t)(context + 128);
    memcpy((void *)(parent - 0x58), &fb, 8);
    memcpy((void *)(parent - 0x48), &vp, 8);
    first[0] = (uintptr_t)second; first[1] = (uintptr_t)module + 0x1273724;
    second[0] = parent; second[1] = (uintptr_t)module + 0x1365e08;
    native[0] = (uintptr_t)first; native[1] = 0;
    sigjmp_buf previous;
    runtime_fault_guard = &previous;
    float basis[4] = {0};
    assert(botw_ui_scissor_basis(native, basis) == 1);
    assert(basis[0] == 100 && basis[1] == 80 && basis[2] == 200 && basis[3] == 160);
    assert(runtime_fault_guard == &previous);
    first[1] += 4;
    assert(botw_ui_scissor_basis(first, basis) == 0);
    first[1] -= 4;
    for (unsigned i = 0; i < sizeof guards / sizeof *guards; i++) {
        uint32_t bad = guards[i][1] ^ 1;
        memcpy(module + guards[i][0], &bad, 4);
        assert(botw_ui_scissor_basis(first, basis) == -2);
        memcpy(module + guards[i][0], &guards[i][1], 4);
    }
    framebuffer[2] = NAN;
    assert(botw_ui_scissor_basis(first, basis) == -3);
    framebuffer[2] = 100;
    viewport[4] = 90;
    assert(botw_ui_scissor_basis(first, basis) == -4);
    viewport[4] = 100; viewport[6] = 1;
    assert(botw_ui_scissor_basis(first, basis) == -4);
    viewport[6] = 0;
    first[0] = (uintptr_t)first;
    assert(botw_ui_scissor_basis(first, basis) == 0);
    signal(SIGSEGV, fault); signal(SIGBUS, fault);
    assert(botw_ui_scissor_basis((uint64_t *)(uintptr_t)0x1000, basis) == -1);
    assert(runtime_fault_guard == &previous);
    runtime_fault_guard = NULL;
    free(module);
}
#endif

int main(void) {
    geometry();
#if defined(GAME_BOTW) && !defined(GAME_BOTW190)
    origin();
#endif
    puts("scissor geometry, caller guards and ABI passed");
    return 0;
}
