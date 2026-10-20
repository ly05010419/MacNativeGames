/* Synthetic data page; the getter and key-3 cycle are the production code. */
#if defined(TOTK_GAME_V143)
#include "../games/totk/runtime/game_patches_v143.c"
#else
#include "../games/totk/runtime/game_patches.c"
#endif
#include <assert.h>

static _Atomic uint64_t page[8];
/* The node runtime_output_mode_set_ex hands the switch to (page[5]). The 1.4.3 layout puts its "switch pending" bit at
   +8, the 1.2.1 layout puts it at +0xca4; this fixture clears whichever the message would make the game's display handler
   clear, so the next switch is accepted. */
static _Atomic uint8_t config[0xcb0];
void runtime_oe_push_message(uint32_t message) {
    (void)message;
    atomic_store((_Atomic uint32_t *)(void *)(config + 8), 0);
    uint32_t flags = atomic_load((_Atomic uint32_t *)(void *)(config + 0xca4));
    atomic_store((_Atomic uint32_t *)(void *)(config + 0xca4), flags & ~0x200u);
}
void shortcut_test_output_init(void) {
    assert(runtime_output_size_current() == pack_size(1920, 1080));
    output_w = 2560; output_h = 1440;
    atomic_store(&page[4], pack_size(1920, 1080));
    atomic_store(&page[5], (uint64_t)(uintptr_t)config);
    atomic_store(&output_page, (uint8_t *)page);
}
void shortcut_test_output_size(unsigned w, unsigned h) {
    atomic_store(&page[4], pack_size(w, h));
    assert(runtime_output_size_current() == pack_size(w, h));
}
