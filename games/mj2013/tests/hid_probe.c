/* The hid shared memory as the game's libnx reads it: hid_publish.c writes, and libnx's reader (_hidGetStates from
   switchbrew/libnx nx/source/services/hid.c, ISC, copied below) must get the latest samples back in order; the keyboard
   layout (runtime/keyboard_layout.h) gives the console's key usages and controller buttons. */
#include "../runtime/hid_publish.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t libnx_get_states(HidCommonLifoHeader *header, void *in_states, size_t max_states, size_t state_offset, size_t sampling_number_offset, void *states, size_t entrysize, size_t count) {
    int32_t total_entries = (int32_t)atomic_load_explicit((_Atomic uint64_t *)&header->count, memory_order_acquire);
    if (total_entries < 0) total_entries = 0;
    if ((size_t)total_entries > count) total_entries = (int32_t)count;
    int32_t tail = (int32_t)atomic_load_explicit((_Atomic uint64_t *)&header->tail, memory_order_acquire);
    for (int32_t i = 0; i < total_entries; i++) {
        int32_t entrypos = (int32_t)((((tail + (max_states + 1)) - total_entries) + i) % max_states);
        void *state_entry = (void *)((uintptr_t)in_states + entrypos * (state_offset + entrysize));
        void *out_state = (void *)((uintptr_t)states + (total_entries - i - 1) * entrysize);
        void *out_state_prev = (void *)((uintptr_t)states + (total_entries - i) * entrysize);
        uint64_t s0 = atomic_load_explicit((_Atomic uint64_t *)state_entry, memory_order_acquire);
        memcpy(out_state, (void *)((uintptr_t)state_entry + state_offset), entrysize);
        uint64_t s1 = atomic_load_explicit((_Atomic uint64_t *)state_entry, memory_order_acquire);
        if (s0 != s1 || (i > 0 && *((uint64_t *)((uintptr_t)out_state + sampling_number_offset)) - *((uint64_t *)((uintptr_t)out_state_prev + sampling_number_offset)) != 1)) {
            int32_t tmpcount = (int32_t)atomic_load_explicit((_Atomic uint64_t *)&header->count, memory_order_acquire);
            tmpcount = total_entries < tmpcount ? tmpcount : total_entries;
            total_entries = (size_t)tmpcount < count ? tmpcount : (int32_t)count;
            tail = (int32_t)atomic_load_explicit((_Atomic uint64_t *)&header->tail, memory_order_acquire);
            i = -1;
        }
    }
    return (size_t)total_entries;
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
int main(void) {
    HidSharedMemory *m = calloc(1, sizeof *m);
    HbInput in; memset(&in, 0, sizeof in);
    in.touch = 1; in.touch_x = 643; in.touch_y = 372;
    keyboard_layout_apply(38, &in);   /* J: A, and the 'j' key */
    hb_hid_publish(m, &in, 1);
    HidTouchScreenState t[17];
    size_t n = libnx_get_states(&m->touchscreen.lifo.header, m->touchscreen.lifo.storage, 17, offsetof(HidTouchScreenStateAtomicStorage, state), offsetof(HidTouchScreenState, sampling_number), t, sizeof t[0], 17);
    CHECK(n == 1 && t[0].count == 1 && t[0].touches[0].x == 643 && t[0].touches[0].y == 372, "one touch at 643,372 (n=%zu)", n);
    HidNpadCommonState p[17];
    HidNpadInternalState *pro = &m->npad.entries[0].internal_state, *hand = &m->npad.entries[8].internal_state;
    n = libnx_get_states(&pro->full_key_lifo.header, pro->full_key_lifo.storage, 17, offsetof(HidNpadCommonStateAtomicStorage, state), offsetof(HidNpadCommonState, sampling_number), p, sizeof p[0], 1);
    CHECK(n == 1 && (p[0].buttons & 1) && (p[0].attributes & 1) && pro->style_set == 1, "player 1: A held, connected, full key");
    n = libnx_get_states(&hand->handheld_lifo.header, hand->handheld_lifo.storage, 17, offsetof(HidNpadCommonStateAtomicStorage, state), offsetof(HidNpadCommonState, sampling_number), p, sizeof p[0], 1);
    CHECK(n == 1 && (p[0].buttons & 1) && hand->style_set == 2, "handheld: A held");
    HidKeyboardState k[2];
    n = libnx_get_states(&m->keyboard.lifo.header, m->keyboard.lifo.storage, 17, offsetof(HidKeyboardStateAtomicStorage, state), offsetof(HidKeyboardState, sampling_number), k, sizeof k[0], 1);
    CHECK(n == 1 && (k[0].keys[13 / 64] >> (13 % 64) & 1), "keyboard: usage 13 (j) held");
    for (uint64_t s = 2; s <= 40; s++) { in.touch_x = (float)s; hb_hid_publish(m, &in, s); }
    n = libnx_get_states(&m->touchscreen.lifo.header, m->touchscreen.lifo.storage, 17, offsetof(HidTouchScreenStateAtomicStorage, state), offsetof(HidTouchScreenState, sampling_number), t, sizeof t[0], 17);
    CHECK(n == 16, "a full LIFO gives 16 samples (got %zu)", n);
    int order = 1; for (size_t i = 0; i < n; i++) order &= t[i].sampling_number == 40 - i && t[i].touches[0].x == 40 - i;
    CHECK(order, "newest first: 40, 39, ...");
    in.touch_x = -5; in.touch_y = 9000; hb_hid_publish(m, &in, 41);
    libnx_get_states(&m->touchscreen.lifo.header, m->touchscreen.lifo.storage, 17, offsetof(HidTouchScreenStateAtomicStorage, state), offsetof(HidTouchScreenState, sampling_number), t, sizeof t[0], 1);
    CHECK(t[0].touches[0].x == 0 && t[0].touches[0].y == 719, "touch clamped to the screen");
    /* the layout */
    HbInput o; memset(&o, 0, sizeof o);
    CHECK(keyboard_hid_usage(0) == 4 && keyboard_hid_usage(6) == 29 && keyboard_hid_usage(29) == 39 && keyboard_hid_usage(36) == 40 && keyboard_hid_usage(53) == 41 && keyboard_hid_usage(123) == 80, "a z 0 Return Escape Left");
    keyboard_layout_apply(56, &o); CHECK((o.modifiers & MOD_SHIFT) && (o.buttons & PAD_ZR), "Shift: modifier and ZR");
    memset(&o, 0, sizeof o); keyboard_layout_apply(126, &o); keyboard_layout_apply(36, &o); CHECK((o.buttons & PAD_UP) && (o.buttons & PAD_PLUS), "Up and Return: d-pad up and +");
    memset(&o, 0, sizeof o); keyboard_layout_apply(13, &o); keyboard_layout_apply(0, &o); CHECK(o.ly == STICK_MAX && o.lx == -STICK_MAX, "W and A: left stick");
    hb_hid_publish(m, &o, 42);
    libnx_get_states(&pro->full_key_lifo.header, pro->full_key_lifo.storage, 17, offsetof(HidNpadCommonStateAtomicStorage, state), offsetof(HidNpadCommonState, sampling_number), p, sizeof p[0], 1);
    CHECK((p[0].buttons & (1u << 16)) && (p[0].buttons & (1u << 17)), "stick left and up buttons from the stick");
    printf(fails ? "hid FAILED\n" : "hid ok\n"); return fails != 0;
}
