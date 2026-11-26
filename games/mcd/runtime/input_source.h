/* Port-local input-source policy. Compatibility is the migration default;
   exclusive modes never fall back to another source when a device is absent. */
#pragma once
enum { INPUT_SOURCE_COMPAT, INPUT_SOURCE_PAD, INPUT_SOURCE_KEYBOARD,
       INPUT_SOURCE_KEYBOARD_MOUSE, INPUT_SOURCE_TOUCH, INPUT_SOURCE_OFF, INPUT_SOURCE_COUNT };
enum { INPUT_DEVICE_PAD=1, INPUT_DEVICE_KEYBOARD=2, INPUT_DEVICE_MOUSE=4, INPUT_DEVICE_TOUCH=8 };
static inline unsigned input_source_mask(int source) {
    static const unsigned masks[INPUT_SOURCE_COUNT] = {15,1,2,6,8,0};
    return source >= 0 && source < INPUT_SOURCE_COUNT ? masks[source] : 0;
}
