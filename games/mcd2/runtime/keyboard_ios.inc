#include "controller_topology.h"
/* iOS exposes one coalesced keyboard, not one GCDevice per physical keyboard.
   Main queue owns configuration and callbacks; the guest reads under kbLock. */
#include "keyboard_map.h"
#include "input_source.h"
#ifndef KEYBOARD_CONFIG_CHANGED
#define KEYBOARD_CONFIG_CHANGED() ((void)0)
#endif
static pthread_mutex_t kbLock = PTHREAD_MUTEX_INITIALIZER;
static KeyboardMap kbMaps[IOS_KEYBOARD_SLOTS];
static KeyboardStateMap kbState;
static int kbSlot = -1, kbConnected, kbSuspended, kbInitialized, kbObserving;
#if !TARGET_OS_OSX
static GCKeyboard *kbDevice;
#endif
#if TARGET_OS_OSX && defined(KEYBOARD_PHYSICAL_HID)
static void physical_keyboard_clear_locked(void);
static void physical_keyboard_merge_locked(int slot, RuntimeGamepadState *out);
static int physical_keyboard_enabled_locked(void);
#endif
static NSString *kb_path(void) {
    const char *controls = getenv("TOTK_CONTROLS");
    return controls && *controls ? [[@(controls) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"keyboard-slots.plist"] : nil;
}
static void kb_load(void) {
    if (kbInitialized) return;
    kbInitialized = 1;
    for (int s = 0; s < IOS_KEYBOARD_SLOTS; s++) keyboard_map_default(&kbMaps[s]);
    NSString *path = kb_path();
    NSDictionary *d = path ? [NSDictionary dictionaryWithContentsOfFile:path] : nil;
    if (![d isKindOfClass:NSDictionary.class] || ![d[@"version"] isEqual:@1]) return;
    NSNumber *slot = d[@"slot"]; NSArray *maps = d[@"maps"];
    if (![slot isKindOfClass:NSNumber.class] || slot.doubleValue != slot.intValue || slot.intValue < -1 || slot.intValue >= IOS_KEYBOARD_SLOTS ||
        ![maps isKindOfClass:NSArray.class] || maps.count != IOS_KEYBOARD_SLOTS) return;
    KeyboardMap next[IOS_KEYBOARD_SLOTS];
    for (int s = 0; s < IOS_KEYBOARD_SLOTS; s++) {
        NSArray *keys = maps[s]; if (![keys isKindOfClass:NSArray.class] || keys.count != KEYBOARD_ACTIONS) return;
        for (int a = 0; a < KEYBOARD_ACTIONS; a++) {
            NSNumber *key = keys[a];
            if (![key isKindOfClass:NSNumber.class] || key.doubleValue != key.intValue || !keyboard_map_set(&next[s], a, key.intValue)) return;
        }
    }
    memcpy(kbMaps, next, sizeof next); kbSlot = slot.intValue;
}
static int kb_save(int slot, const KeyboardMap *maps) {
    NSString *path = kb_path(); if (!path) return 0;
    NSMutableArray *values = [NSMutableArray array];
    for (int s = 0; s < IOS_KEYBOARD_SLOTS; s++) {
        NSMutableArray *keys = [NSMutableArray array];
        for (int a = 0; a < KEYBOARD_ACTIONS; a++) [keys addObject:@(maps[s].key[a])];
        [values addObject:keys];
    }
    NSDictionary *d = @{@"version":@1, @"slot":@(slot), @"maps":values};
    if (![d writeToFile:path atomically:YES]) return 0;
    pthread_mutex_lock(&kbLock);
    if (maps != kbMaps) memcpy(kbMaps, maps, sizeof kbMaps); kbSlot = slot; keyboard_state_clear(&kbState);
#if TARGET_OS_OSX && defined(KEYBOARD_PHYSICAL_HID)
    physical_keyboard_clear_locked();
#endif
    pthread_mutex_unlock(&kbLock);
    KEYBOARD_CONFIG_CHANGED();
    return 1;
}
int runtime_keyboard_slot(void) { kb_load(); return kbSlot; }
int runtime_keyboard_slot_count(void) {
    /* Handheld games expose only the paired handheld controller, never fake P2. */
    return d3_handheld_controller() ? 1 : IOS_KEYBOARD_SLOTS;
}
int runtime_keyboard_bind(int slot) {
    kb_load(); if (slot < -1 || slot >= runtime_keyboard_slot_count()) return 0;
    return kb_save(slot, kbMaps);
}
int runtime_keyboard_mapping_get(int slot, int action) {
    kb_load(); return slot >= 0 && slot < IOS_KEYBOARD_SLOTS && action >= 0 && action < KEYBOARD_ACTIONS ? kbMaps[slot].key[action] : -1;
}
int runtime_keyboard_mapping_set(int slot, int action, int key) {
    kb_load(); if (slot < 0 || slot >= IOS_KEYBOARD_SLOTS) return 0;
    KeyboardMap next[IOS_KEYBOARD_SLOTS]; memcpy(next, kbMaps, sizeof next);
    if (!keyboard_map_set(&next[slot], action, key)) return 0;
    return kb_save(kbSlot, next);
}
int runtime_keyboard_mapping_reset(int slot) {
    kb_load(); if (slot < 0 || slot >= IOS_KEYBOARD_SLOTS) return 0;
    KeyboardMap next[IOS_KEYBOARD_SLOTS]; memcpy(next, kbMaps, sizeof next); keyboard_map_default(&next[slot]);
    return kb_save(kbSlot, next);
}
void runtime_keyboard_clear(void) {
    pthread_mutex_lock(&kbLock); keyboard_state_clear(&kbState);
#if TARGET_OS_OSX && defined(KEYBOARD_PHYSICAL_HID)
    physical_keyboard_clear_locked();
#endif
    pthread_mutex_unlock(&kbLock);
}
void runtime_keyboard_suspend(int suspended) {
    pthread_mutex_lock(&kbLock); kbSuspended = !!suspended; keyboard_state_clear(&kbState);
#if TARGET_OS_OSX && defined(KEYBOARD_PHYSICAL_HID)
    physical_keyboard_clear_locked();
#endif
    pthread_mutex_unlock(&kbLock);
}
#if !TARGET_OS_OSX
static void kb_attach(GCKeyboard *device) {
    kbDevice.keyboardInput.keyChangedHandler = nil; kbDevice = device;
    pthread_mutex_lock(&kbLock); kbConnected = device != nil; keyboard_state_clear(&kbState); pthread_mutex_unlock(&kbLock);
    KEYBOARD_CONFIG_CHANGED();
    device.handlerQueue = dispatch_get_main_queue();
    device.keyboardInput.keyChangedHandler = ^(GCKeyboardInput *input, GCDeviceButtonInput *button, GCKeyCode code, BOOL down) {
        (void)button;
        if (input != kbDevice.keyboardInput || UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
        pthread_mutex_lock(&kbLock);
        if (!kbSuspended && runtime_input_source_allows(kbSlot, INPUT_DEVICE_KEYBOARD))
            keyboard_state_event(&kbState, &kbMaps[kbSlot], (int)code, down);
        pthread_mutex_unlock(&kbLock);
    };
}
static void kb_start(void) {
    kb_load(); if (kbObserving) return; kbObserving = 1;
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:GCKeyboardDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { kb_attach(n.object); }];
    [nc addObserverForName:GCKeyboardDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { if (n.object == kbDevice) kb_attach(nil); }];
    [nc addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        (void)n; pthread_mutex_lock(&kbLock); keyboard_state_clear(&kbState); pthread_mutex_unlock(&kbLock);
    }];
    kb_attach(GCKeyboard.coalescedKeyboard);
}
#else
/* AppKit window events are intentionally used here, not GCKeyboard callbacks:
   otherwise the old window input and the coalesced GC event can fire twice.
   This is one system keyboard, not independent physical keyboards. */
#include "keyboard_macos.h"
static void kb_start(void) { kb_load(); kbObserving = 1; kbConnected = 1; }
int runtime_keyboard_mac_key(unsigned code, int down) {
    kb_load();
    pthread_mutex_lock(&kbLock);
#if defined(KEYBOARD_PHYSICAL_HID)
    if (physical_keyboard_enabled_locked()) { pthread_mutex_unlock(&kbLock); return 1; }
#endif
    /* A saved binding is not an enabled input source. */
    int assigned = runtime_input_source_allows(kbSlot, INPUT_DEVICE_KEYBOARD);
    if (assigned && !kbSuspended)
        keyboard_state_event(&kbState, &kbMaps[kbSlot], keyboard_macos_usage(code), down);
    pthread_mutex_unlock(&kbLock);
    return assigned;
}
#endif
static void kb_merge(int slot, RuntimeGamepadState *out) {
    if (!runtime_input_source_allows(slot, INPUT_DEVICE_KEYBOARD)) return;
    pthread_mutex_lock(&kbLock);
#if TARGET_OS_OSX && defined(KEYBOARD_PHYSICAL_HID)
    if (physical_keyboard_enabled_locked()) {
        physical_keyboard_merge_locked(slot, out); pthread_mutex_unlock(&kbLock); return;
    }
#endif
    if (slot == kbSlot && kbConnected && !kbSuspended) {
        KeyboardOutput k = keyboard_state_read(&kbState, &kbMaps[slot]);
        out->buttons |= k.buttons; out->connected = 1;
        if (abs(k.lx) > abs(out->lx)) out->lx = k.lx;
        if (abs(k.ly) > abs(out->ly)) out->ly = k.ly;
        if (abs(k.rx) > abs(out->rx)) out->rx = k.rx;
        if (abs(k.ry) > abs(out->ry)) out->ry = k.ry;
    }
    pthread_mutex_unlock(&kbLock);
}
