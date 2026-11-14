/* One HID object's lifetime, independent of logical controller identity.
   A binding becomes a retirement lease before it is detached. That lease is released only after its producer
   queue and output queue have drained. Rebinding the same object adds another binding; it does not erase old work.
   All transitions run on main. A close is permitted exactly once, after BOTH counts reach zero. */
#pragma once
#include <limits.h>

typedef struct { unsigned bindings, retiring; int closed; } HidDeviceLifetime;

static inline int hid_lifetime_bind(HidDeviceLifetime *s) {
    if (s->closed || s->bindings == UINT_MAX) return 0;
    ++s->bindings;
    return 1;
}
static inline int hid_lifetime_retire(HidDeviceLifetime *s) {
    if (s->closed || !s->bindings || s->retiring == UINT_MAX) return 0;
    --s->bindings; ++s->retiring;
    return 1;
}
static inline int hid_lifetime_finish(HidDeviceLifetime *s) {
    if (s->closed || !s->retiring) return 0;
    --s->retiring;
    return 1;
}
static inline int hid_lifetime_close(HidDeviceLifetime *s) {
    if (s->closed || s->bindings || s->retiring) return 0;
    s->closed = 1;
    return 1;
}
