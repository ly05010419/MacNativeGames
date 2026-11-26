/* Main queue only. GameController has no public persistent hardware identifier.
   Give each connection object a UUID, never a vendor-name identity. A new object
   after reconnect must be reselected; persisted reservations remain offline.
   This intentionally does not use private KVC identifier/deviceHash APIs. */
#include "controller_assignment.h"
#ifndef DEVICE_SLOT_ACCEPTS_PAD
#define DEVICE_SLOT_ACCEPTS_PAD(s) 1
#endif
static NSMapTable<GCController *, NSString *> *deviceIDs;
static NSMutableDictionary<NSString *, NSString *> *deviceNames;
static NSString *deviceBindings[RUNTIME_MAX_GAMEPADS];
static NSString *deviceBindingNames[RUNTIME_MAX_GAMEPADS];
static int deviceBindingsLoaded;
static NSString *device_id(GCController *controller) {
    if (!deviceIDs) {
        deviceIDs = [NSMapTable weakToStrongObjectsMapTable];
        deviceNames = [NSMutableDictionary dictionary];
    }
    NSString *key = [deviceIDs objectForKey:controller];
    if (!key) {
        key = [@"session:" stringByAppendingString:NSUUID.UUID.UUIDString];
        [deviceIDs setObject:key forKey:controller];
        deviceNames[key] = [NSString stringWithFormat:@"%@ · %@", controller.vendorName ?: @"手柄", [key substringFromIndex:key.length-6]];
    }
    return key;
}
static NSString *device_path(void) {
    char path[4096];
    return setting_path("controller-devices.plist", path, sizeof path) ? @(path) : nil;
}
static BOOL device_key_valid(NSString *key) {
    if (![key isKindOfClass:NSString.class]) return NO;
    if ([key isEqual:@"auto"] || [key isEqual:@"none"]) return YES;
    return [key hasPrefix:@"session:"] && [[NSUUID alloc] initWithUUIDString:[key substringFromIndex:8]] != nil;
}
static void device_load(void) {
    if (deviceBindingsLoaded) return;
    deviceBindingsLoaded = 1;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) { deviceBindings[s] = @"auto"; deviceBindingNames[s] = @""; }
    NSString *path = device_path();
    NSDictionary *root = path ? [NSDictionary dictionaryWithContentsOfFile:path] : nil;
    if (![root isKindOfClass:NSDictionary.class] || ![root[@"version"] isEqual:@1]) return;
    NSArray *ports = root[@"ports"];
    if (![ports isKindOfClass:NSArray.class] || ports.count != RUNTIME_MAX_GAMEPADS) return;
    NSMutableSet *owned = [NSMutableSet set];
    for (id port in ports) {
        if (![port isKindOfClass:NSDictionary.class] || !device_key_valid(port[@"id"]) ||
            ![port[@"name"] isKindOfClass:NSString.class] || [port[@"name"] length] > 512) return;
        NSString *key = port[@"id"];
        if ([key hasPrefix:@"session:"]) {
            if ([owned containsObject:key]) return;
            [owned addObject:key];
        }
    }
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) {
        deviceBindings[s] = ports[s][@"id"]; deviceBindingNames[s] = ports[s][@"name"];
    }
}
static NSArray<GCController *> *device_list(void) {
    NSMutableArray *list = [NSMutableArray array];
    for (GCController *c in GCController.controllers) if (c.extendedGamepad) { device_id(c); [list addObject:c]; }
    return list;
}
/* Snapshot tokens are copied by the UI into each menu item, never array indices
   that may refer to a different controller when the user eventually clicks. */
NSArray<NSDictionary *> *runtime_gamepad_devices(void) {
    NSMutableArray *devices = [NSMutableArray array];
    for (GCController *c in device_list()) {
        NSString *key = device_id(c);
        [devices addObject:@{@"id":key, @"name":deviceNames[key]}];
    }
    return devices;
}
NSString *runtime_gamepad_device_binding(int slot) {
    device_load(); return slot_valid(slot) ? deviceBindings[slot] : nil;
}
NSString *runtime_gamepad_device_label(int slot) {
    device_load(); if (!slot_valid(slot)) return @"无效玩家";
    NSString *key = deviceBindings[slot];
    if ([key isEqual:@"auto"]) return @"自动分配手柄";
    if ([key isEqual:@"none"]) return @"不使用实体手柄";
    for (GCController *c in device_list()) if ([device_id(c) isEqual:key]) return deviceNames[key];
    return [NSString stringWithFormat:@"离线：%@（请重新选择）",deviceBindingNames[slot]];
}
int runtime_gamepad_device_bind(int slot, NSString *key) {
    device_load(); slot_load();
    if (!slot_valid(slot) || !device_key_valid(key)) return 0;
#if TARGET_OS_OSX
    /* Raw-HID backend still owns its hardware globally. Never pretend that its
       devices have been independently assigned through this GameController UI. */
    if (atomic_load(&controllerType) == GAMEPAD_TYPE_THIRDPARTY_PRO) return 0;
#endif
    NSString *name = @"";
    if ([key hasPrefix:@"session:"]) {
        BOOL connected = NO;
        for (GCController *c in device_list()) if ([device_id(c) isEqual:key]) { connected = YES; name = deviceNames[key]; }
        if (!connected) return 0;
        for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s)
            if (s != slot && [deviceBindings[s] isEqual:key]) return 0;
    }
    NSMutableArray *ports = [NSMutableArray array];
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s)
        [ports addObject:@{@"id":s == slot ? key : deviceBindings[s], @"name":s == slot ? name : deviceBindingNames[s]}];
    NSString *path = device_path();
    if (!path || ![@{@"version":@1, @"ports":ports} writeToFile:path atomically:YES]) return 0;
    deviceBindings[slot] = [key copy]; deviceBindingNames[slot] = name;
    if (started) refresh_gamepads();
    return 1;
}
static BOOL device_slot_auto(int slot) {
    device_load();
    if (!DEVICE_SLOT_ACCEPTS_PAD(slot)) return NO;
#if TARGET_OS_OSX
    /* Global compatibility mode temporarily bypasses GC session reservations.
       Keep them on disk for restoration when leaving raw mode; honor "none". */
    if (atomic_load(&controllerType) == GAMEPAD_TYPE_THIRDPARTY_PRO)
        return ![deviceBindings[slot] isEqual:@"none"];
#endif
    return [deviceBindings[slot] isEqual:@"auto"];
}
static NSArray *device_resolve(NSArray<GCController *> *controllers) {
    device_load();
    NSUInteger count = controllers.count;
    /* Controllers are few but their count is external; use bounded stack only
       for the fixed ports and dynamic storage for device identities. */
    const char **ids = calloc(count ? count : 1, sizeof *ids);
    if (!ids) runtime_fail("controller assignment allocation");
    for (NSUInteger d = 0; d < count; ++d) ids[d] = device_id(controllers[d]).UTF8String;
    const char *bindings[RUNTIME_MAX_GAMEPADS]; int previous[RUNTIME_MAX_GAMEPADS], result[RUNTIME_MAX_GAMEPADS];
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) {
        bindings[s] = DEVICE_SLOT_ACCEPTS_PAD(s) ? deviceBindings[s].UTF8String : "none";
        NSUInteger p = gamepadSlots[s] ? [controllers indexOfObjectIdenticalTo:gamepadSlots[s]] : NSNotFound;
        previous[s] = p == NSNotFound ? -1 : (int)p;
    }
    controller_assignment_resolve(RUNTIME_MAX_GAMEPADS, bindings, (int)count, ids, previous, result);
    free(ids);
    NSMutableArray *resolved = [NSMutableArray array];
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s)
        [resolved addObject:result[s] < 0 ? (id)NSNull.null : controllers[result[s]]];
    return resolved;
}
