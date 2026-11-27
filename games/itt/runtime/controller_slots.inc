#include "controller_topology.h"
/* Main event queue only. Legacy controls.txt seeds every port on first use;
   subsequent edits persist independently, without rewriting that source file. */
static void slot_clear(int slot) {
    memset(&slotModeState[slot], 0, sizeof slotModeState[slot]);
    pthread_mutex_lock(&gamepadLock);
    gamepadStates[slot] = (RuntimeGamepadState){0};
    pressedEdgesSlot[slot] = 0;
    if (!slot) { gamepadState = (RuntimeGamepadState){0}; pressedEdges = 0; }
    pthread_mutex_unlock(&gamepadLock);
}
static NSString *slot_path(void) {
    char path[4096];
    return setting_path("controller-slots.plist", path, sizeof path) ? @(path) : nil;
}
static void slot_load(void) {
    if (slotsInitialized) return;
    if (!started) { load_controller_map(); load_controller_type(); }
    slotsInitialized = 1;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) {
        memcpy(slotMap[s], controllerMap, sizeof controllerMap);
        memcpy(slotModes[s], controllerModes, sizeof controllerModes);
        slotTypes[s] = atomic_load(&controllerType);
    }
    NSString *path = slot_path();
    NSDictionary *root = path ? [NSDictionary dictionaryWithContentsOfFile:path] : nil;
    if (![root isKindOfClass:NSDictionary.class] || ![root[@"version"] isEqual:@1]) return;
    NSArray *ports = root[@"ports"];
    if (![ports isKindOfClass:NSArray.class] || ports.count != RUNTIME_MAX_GAMEPADS) return;
    uint16_t maps[RUNTIME_MAX_GAMEPADS][CONTROLLER_BUTTONS];
    uint8_t modes[RUNTIME_MAX_GAMEPADS][CONTROLLER_BUTTONS];
    int types[RUNTIME_MAX_GAMEPADS];
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) {
        NSDictionary *port = ports[s];
        if (![port isKindOfClass:NSDictionary.class]) return;
        NSNumber *type = port[@"type"]; NSString *layout = port[@"layout"];
        if (![type isKindOfClass:NSNumber.class] || type.doubleValue != type.intValue ||
            type.intValue < 0 || type.intValue >= GAMEPAD_TYPE_COUNT || ![layout isKindOfClass:NSString.class]) return;
        controller_map_default(maps[s], modes[s]);
        const char *error = NULL;
        if (controller_map_parse(layout.UTF8String, maps[s], modes[s], &error)) return;
        types[s] = type.intValue;
    }
    memcpy(slotMap, maps, sizeof maps); memcpy(slotModes, modes, sizeof modes);
    memcpy(slotTypes, types, sizeof types);
    atomic_store(&controllerType, slotTypes[0]);
}
static int slot_save(int slot, const uint16_t *map, const uint8_t *modes, int type) {
    NSMutableArray *ports = [NSMutableArray array];
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) {
        char text[4096];
        if (!controller_map_format(text, sizeof text, s == slot ? map : slotMap[s],
                                  s == slot ? modes : slotModes[s])) return 0;
        [ports addObject:@{@"type":@(s == slot ? type : slotTypes[s]), @"layout":@(text)}];
    }
    NSString *path = slot_path();
    if (!path || ![@{@"version":@1, @"ports":ports} writeToFile:path atomically:YES]) return 0;
    if (map != slotMap[slot]) memcpy(slotMap[slot], map, sizeof slotMap[slot]);
    if (modes != slotModes[slot]) memcpy(slotModes[slot], modes, sizeof slotModes[slot]);
    int previousType = slotTypes[slot];
    slotTypes[slot] = type;
    if (!slot) atomic_store(&controllerType, type);
    slot_clear(slot);
#ifdef SLOT_BACKEND_CHANGED
    SLOT_BACKEND_CHANGED(slot, previousType, type);
#else
    (void)previousType;
#endif
    if (started) refresh_gamepads();
    return 1;
}
static int slot_valid(int slot) { return slot >= 0 && slot < RUNTIME_MAX_GAMEPADS; }
int runtime_gamepad_slot_count(void) { return d3_handheld_controller() ? 1 : RUNTIME_MAX_GAMEPADS; }
int runtime_gamepad_slot_type(int slot) {
    slot_load(); return slot_valid(slot) ? slotTypes[slot] : -1;
}
int runtime_gamepad_slot_type_set(int slot, int type) {
    slot_load();
    if (!slot_valid(slot) || type < 0 || type >= GAMEPAD_TYPE_COUNT) return 0;
#if TARGET_OS_OSX
    /* The explicit global compatibility switch is stored on P1 only. */
    if (type == GAMEPAD_TYPE_THIRDPARTY_PRO && slot != 0) return 0;
#endif
    return slot_save(slot, slotMap[slot], slotModes[slot], type);
}
int runtime_gamepad_slot_mapping_get(int slot, int physical) {
    slot_load();
    if (!slot_valid(slot) || physical < 0 || physical >= CONTROLLER_BUTTONS) return -1;
    uint16_t bits = slotMap[slot][physical];
    return bits ? __builtin_ctz((unsigned)bits) : -1;
}
int runtime_gamepad_slot_mapping_set(int slot, int physical, int target) {
    slot_load();
    if (!slot_valid(slot) || physical < 0 || physical >= CONTROLLER_BUTTONS || target < -1 || target >= 16) return 0;
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS];
    memcpy(map, slotMap[slot], sizeof map); memcpy(modes, slotModes[slot], sizeof modes);
    map[physical] = target < 0 ? 0 : (uint16_t)(1u << target); modes[physical] = CONTROLLER_HOLD;
    return slot_save(slot, map, modes, slotTypes[slot]);
}
int runtime_gamepad_slot_mapping_reset(int slot) {
    slot_load(); if (!slot_valid(slot)) return 0;
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS];
    controller_map_default(map, modes);
    for (int i = 16; i < CONTROLLER_BUTTONS; ++i) { map[i] = slotMap[slot][i]; modes[i] = slotModes[slot][i]; }
    return slot_save(slot, map, modes, slotTypes[slot]);
}
int runtime_gamepad_mapping_get(int physical) { return runtime_gamepad_slot_mapping_get(0, physical); }
int runtime_gamepad_mapping_set(int physical, int target) { return runtime_gamepad_slot_mapping_set(0, physical, target); }
int runtime_gamepad_mapping_reset(void) { return runtime_gamepad_slot_mapping_reset(0); }

/* Target-first view over the existing physical maps. One atomic save removes
   the old inputs for this target within this device group and installs the new
   input. Other groups, players and untouched button modes remain unchanged. */
int runtime_input_binding_get(int slot, int group, int target) {
    slot_load();
    if (!slot_valid(slot) || group < 0 || group > 1 || target < 0 || target >= 16) return -1;
    int first = group ? 17 : 0, end = group ? CONTROLLER_BUTTONS : 17, found = -1;
    for (int i = first; i < end; ++i) if (slotMap[slot][i] & (1u << target)) {
        if (found >= 0) return -2;
        found = i;
    }
    return found;
}
int runtime_input_binding_set(int slot, int group, int target, int physical) {
    slot_load();
    int first = group ? 17 : 0, end = group ? CONTROLLER_BUTTONS : 17;
    if (!slot_valid(slot) || group < 0 || group > 1 || target < 0 || target >= 16 ||
        (physical != -1 && (physical < first || physical >= end)) || (group && slot != 0)) return 0;
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS];
    memcpy(map, slotMap[slot], sizeof map); memcpy(modes, slotModes[slot], sizeof modes);
    for (int i = first; i < end; ++i) if (map[i] & (1u << target)) {
        map[i] &= ~(1u << target); modes[i] = CONTROLLER_HOLD;
    }
    if (physical >= 0) { map[physical] = (uint16_t)(1u << target); modes[physical] = CONTROLLER_HOLD; }
    return slot_save(slot, map, modes, slotTypes[slot]);
}
int runtime_input_binding_reset(int slot, int group) {
    slot_load(); if (!slot_valid(slot) || group < 0 || group > 1 || (group && slot != 0)) return 0;
    uint16_t map[CONTROLLER_BUTTONS], defaults[CONTROLLER_BUTTONS];
    uint8_t modes[CONTROLLER_BUTTONS], defaultModes[CONTROLLER_BUTTONS];
    memcpy(map, slotMap[slot], sizeof map); memcpy(modes, slotModes[slot], sizeof modes);
    controller_map_default(defaults, defaultModes);
    int first = group ? 17 : 0, end = group ? CONTROLLER_BUTTONS : 17;
    for (int i = first; i < end; ++i) { map[i] = defaults[i]; modes[i] = defaultModes[i]; }
    return slot_save(slot, map, modes, slotTypes[slot]);
}
