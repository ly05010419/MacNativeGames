// Wwise finds its span header by masking an interior pointer returned from Unreal's heap (#996).
#include "../hle_uemalloc.c"
#include <assert.h>

module_t *g_mod;
int g_nmod;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
int gate_obs_present(int id) { (void)id; return 0; }
void gate_obs_set(int id, void (*callback)(uint64_t *, int)) { (void)id; (void)callback; }

int main(void) {
    enum { span = 65536, count = 16 };
    uint8_t *blocks[count];
    g_zone = malloc_create_zone(0, 0);
    assert(g_zone);
    for (unsigned i = 0; i < count; ++i) {
        blocks[i] = m_Malloc(NULL, span + (i % 2) * 4096, (i % 4 < 2) ? 4096 : 0);
        assert(((uintptr_t)(blocks[i] + 0x2a0) & ~(uintptr_t)(span - 1)) == (uintptr_t)blocks[i]);
        blocks[i][0x2a0] = (uint8_t)(i + 1);
    }
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *grown = m_Realloc(NULL, blocks[i], 3 * span + 4096, 0);
        assert(((uintptr_t)grown & (span - 1)) == 0 && grown[0x2a0] == i + 1);
        uint8_t *shrunk = m_Realloc(NULL, grown, span, 2 * span);
        assert(((uintptr_t)shrunk & (2 * span - 1)) == 0 && shrunk[0x2a0] == i + 1);
        m_Free(NULL, shrunk);
    }
    uint8_t *small = m_Malloc(NULL, 48, 0);
    small[47] = 0x5a;
    uint8_t *span_block = m_Realloc(NULL, small, span + 4096, 0);
    assert(((uintptr_t)span_block & (span - 1)) == 0 && span_block[47] == 0x5a);
    m_Free(NULL, span_block);
    // A block just under the threshold can grow across it in place, off the boundary: it must be moved with its contents
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *under = m_Malloc(NULL, span - 4096, 0);
        memset(under, (int)(i + 1), span - 4096);
        uint8_t *over = m_Realloc(NULL, under, span + 4096, 0);
        assert(((uintptr_t)over & (span - 1)) == 0 && over[0] == i + 1 && over[span - 4097] == i + 1);
        m_Free(NULL, over);
    }
    // A shrink gives the memory back, at either alignment (keeping the old block would hold its pages for the block's life)
    for (uint32_t alignment = 0; alignment <= 4096; alignment += 4096) {
        enum { big = 8 << 20, kept = 2 * span };
        uint8_t *large = m_Malloc(NULL, big, alignment);
        large[kept - 1] = 0x77;
        uint8_t *less = m_Realloc(NULL, large, kept, alignment);
        assert(((uintptr_t)less & (span - 1)) == 0 && less[kept - 1] == 0x77);
        assert(malloc_size(less) < big);
        m_Free(NULL, less);
    }
    assert(atomic_load(&g_used) == 0);
    malloc_destroy_zone(g_zone);
    puts("Unreal virtual-span alignment and realloc contents passed");
    return 0;
}
