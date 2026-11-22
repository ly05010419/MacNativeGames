/* See hid_publish.h. */
#include "hid_publish.h"
#include <stdatomic.h>
#include <string.h>

/* One entry pushed into a LIFO: storage[tail+1] is written (its sampling number last, as the reader checks it on both
   sides of its copy), then the tail moves and the count grows to at most 16 of its 17 entries. */
void hb_lifo_push(HidCommonLifoHeader *h, void *storage, size_t entry_size, const void *state, size_t state_size, uint64_t number) {
    uint64_t next = (atomic_load_explicit((_Atomic uint64_t *)&h->tail, memory_order_relaxed) + 1) % 17;
    uint8_t *e = (uint8_t *)storage + next * entry_size;
    atomic_store_explicit((_Atomic uint64_t *)e, number, memory_order_relaxed);
    memcpy(e + 8, state, state_size);
    atomic_store_explicit((_Atomic uint64_t *)e, number, memory_order_release);
    h->buffer_count = 17;
    atomic_store_explicit((_Atomic uint64_t *)&h->tail, next, memory_order_release);
    uint64_t c = atomic_load_explicit((_Atomic uint64_t *)&h->count, memory_order_relaxed);
    if (c < 16) atomic_store_explicit((_Atomic uint64_t *)&h->count, c + 1, memory_order_release);
}
static uint64_t stick_bits(const HbInput *in) {   /* the stick direction buttons Horizon derives from the left stick */
    uint64_t b = 0, half = STICK_MAX / 2;
    if (in->lx < -(int32_t)half) b |= 1u << 16; if (in->ly > (int32_t)half) b |= 1u << 17;
    if (in->lx > (int32_t)half) b |= 1u << 18;  if (in->ly < -(int32_t)half) b |= 1u << 19;
    return b;
}
void hb_hid_publish(HidSharedMemory *m, const HbInput *in, uint64_t n) {
    HidTouchScreenState t; memset(&t, 0, sizeof t); t.sampling_number = n;
    if (in->touch) {
        t.count = 1; t.touches[0].attributes = 0; t.touches[0].finger_id = 0;
        t.touches[0].x = (uint32_t)(in->touch_x < 0 ? 0 : in->touch_x > 1279 ? 1279 : in->touch_x);
        t.touches[0].y = (uint32_t)(in->touch_y < 0 ? 0 : in->touch_y > 719 ? 719 : in->touch_y);
        t.touches[0].diameter_x = t.touches[0].diameter_y = 15;
    }
    hb_lifo_push(&m->touchscreen.lifo.header, m->touchscreen.lifo.storage, sizeof m->touchscreen.lifo.storage[0], &t, sizeof t, n);

    HidMouseState ms; memset(&ms, 0, sizeof ms); ms.sampling_number = n;
    static int32_t last_x, last_y;
    ms.x = (int32_t)in->mouse_x; ms.y = (int32_t)in->mouse_y; ms.delta_x = ms.x - last_x; ms.delta_y = ms.y - last_y; last_x = ms.x; last_y = ms.y;
    ms.buttons = (uint32_t)in->mouse_buttons; ms.attributes = in->mouse_connected ? (1u << 1) : 0;
    hb_lifo_push(&m->mouse.lifo.header, m->mouse.lifo.storage, sizeof m->mouse.lifo.storage[0], &ms, sizeof ms, n);

    HidKeyboardState k; memset(&k, 0, sizeof k); k.sampling_number = n; k.modifiers = in->modifiers; memcpy(k.keys, in->keys, sizeof k.keys);
    hb_lifo_push(&m->keyboard.lifo.header, m->keyboard.lifo.storage, sizeof m->keyboard.lifo.storage[0], &k, sizeof k, n);

    HidNpadCommonState p; memset(&p, 0, sizeof p); p.sampling_number = n;
    p.buttons = in->buttons | stick_bits(in);
    p.analog_stick_l.x = in->lx; p.analog_stick_l.y = in->ly; p.analog_stick_r.x = in->rx; p.analog_stick_r.y = in->ry;
    p.attributes = 1 | 2 | 4 | 16;   /* IsConnected, IsWired, IsLeftConnected, IsRightConnected */
    HidNpadInternalState *pro = &m->npad.entries[0].internal_state, *hand = &m->npad.entries[8].internal_state;
    pro->style_set = 1; pro->joy_assignment_mode = 0; pro->device_type = 1;          /* FullKey: a Pro Controller */
    hand->style_set = 2; hand->joy_assignment_mode = 0; hand->device_type = 4 | 8;   /* Handheld: both Joy-Con on the console */
    hb_lifo_push(&pro->full_key_lifo.header, pro->full_key_lifo.storage, sizeof pro->full_key_lifo.storage[0], &p, sizeof p, n);
    hb_lifo_push(&hand->handheld_lifo.header, hand->handheld_lifo.storage, sizeof hand->handheld_lifo.storage[0], &p, sizeof p, n);
}
