/* Physical keyboard adapter. Main run loop owns IOHID devices/configuration;
   kbLock guards only guest-visible state. Never seize devices or record keys.
   Opt-in per process: enumerate/request permission only after the UI action. */
#import <IOKit/hidsystem/IOHIDLib.h>
#import <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>
static IOHIDManagerRef physicalKeyboardManager;
static dispatch_source_t physicalKeyboardPermissionTimer;
static int physicalKeyboardBlocked;
static NSMutableDictionary<NSValue *, NSDictionary *> *physicalKeyboards;
static NSString *physicalKeyboardBinding[IOS_KEYBOARD_SLOTS];
static NSString *physicalKeyboardName[IOS_KEYBOARD_SLOTS];
static KeyboardStateMap physicalKeyboardState[IOS_KEYBOARD_SLOTS];
static int physicalKeyboardConnected[IOS_KEYBOARD_SLOTS];
static int physicalKeyboardEnabled, physicalKeyboardLoaded, physicalKeyboardFocused;
static int physical_keyboard_enabled_locked(void) { return physicalKeyboardEnabled; }
static NSString *physicalKeyboardStatus = @"独立物理键盘未启用；启用需要系统输入监控权限。";
/* Caller holds kbLock. */
static void physical_keyboard_clear_locked(void) {
    memset(physicalKeyboardState, 0, sizeof physicalKeyboardState);
}
static NSString *physical_keyboard_path(void) {
    const char *controls = getenv("TOTK_CONTROLS");
    return controls && *controls ? [[@(controls) stringByDeletingLastPathComponent]
        stringByAppendingPathComponent:@"physical-keyboards.plist"] : nil;
}
static BOOL physical_keyboard_valid_key(id key) {
    return [key isKindOfClass:NSString.class] && [key length] <= 2048 &&
        ([key isEqual:@"none"] || ([key hasPrefix:@"serial:"] && [key length] > 7) ||
         ([key hasPrefix:@"connection:"] && [key length] > 11));
}
static void physical_keyboard_load(void) {
    if (physicalKeyboardLoaded) return;
    physicalKeyboardLoaded = 1;
    for (int s = 0; s < IOS_KEYBOARD_SLOTS; ++s) {
        physicalKeyboardBinding[s] = @"none"; physicalKeyboardName[s] = @"";
    }
    NSString *path = physical_keyboard_path();
    NSDictionary *root = path ? [NSDictionary dictionaryWithContentsOfFile:path] : nil;
    if (![root isKindOfClass:NSDictionary.class] || ![root[@"version"] isEqual:@1]) return;
    NSArray *ports = root[@"ports"];
    if (![ports isKindOfClass:NSArray.class] || ports.count != IOS_KEYBOARD_SLOTS) return;
    NSMutableSet *seen = [NSMutableSet set];
    for (id port in ports) {
        if (![port isKindOfClass:NSDictionary.class] || !physical_keyboard_valid_key(port[@"id"]) ||
            ![port[@"name"] isKindOfClass:NSString.class] || [port[@"name"] length] > 512) return;
        if (![port[@"id"] isEqual:@"none"]) {
            if ([seen containsObject:port[@"id"]]) return;
            [seen addObject:port[@"id"]];
        }
    }
    for (int s = 0; s < IOS_KEYBOARD_SLOTS; ++s) {
        physicalKeyboardBinding[s] = ports[s][@"id"]; physicalKeyboardName[s] = ports[s][@"name"];
    }
}
/* One and only one live HID interface may satisfy a reservation. Devices that
   report duplicated serial numbers are ambiguous and never silently combined. */
static int physical_keyboard_matches(NSString *key) {
    int count = 0;
    for (NSDictionary *info in physicalKeyboards.allValues) if ([info[@"id"] isEqual:key]) ++count;
    return count;
}
static void physical_keyboard_topology(void) {
    int next[IOS_KEYBOARD_SLOTS];
    for (int s = 0; s < IOS_KEYBOARD_SLOTS; ++s)
        next[s] = physicalKeyboardEnabled && !physicalKeyboardBlocked && ![physicalKeyboardBinding[s] isEqual:@"none"] &&
            physical_keyboard_matches(physicalKeyboardBinding[s]) == 1;
    pthread_mutex_lock(&kbLock);
    for (int s = 0; s < IOS_KEYBOARD_SLOTS; ++s) {
        if (physicalKeyboardConnected[s] != next[s]) keyboard_state_clear(&physicalKeyboardState[s]);
        physicalKeyboardConnected[s] = next[s];
    }
    pthread_mutex_unlock(&kbLock);
    KEYBOARD_CONFIG_CHANGED();
}
/* Event core is also used by isolated tests; accepts only already-enumerated
   device tokens. It never logs usages, characters or input content. */
static void physical_keyboard_event(NSValue *token, unsigned page, unsigned usage, int down) {
    if (page != kHIDPage_KeyboardOrKeypad || usage < 4 || usage >= KEYBOARD_KEYS) return;
    NSString *key = physicalKeyboards[token][@"id"];
    if (!key || physical_keyboard_matches(key) != 1) return;
    pthread_mutex_lock(&kbLock);
    if (physicalKeyboardEnabled && !physicalKeyboardBlocked && physicalKeyboardFocused && !kbSuspended)
        for (int s = 0; s < IOS_KEYBOARD_SLOTS; ++s)
            if (runtime_input_source_allows(s, INPUT_DEVICE_KEYBOARD) &&
                physicalKeyboardConnected[s] && [physicalKeyboardBinding[s] isEqual:key])
                keyboard_state_event(&physicalKeyboardState[s], &kbMaps[s], (int)usage, down);
    pthread_mutex_unlock(&kbLock);
}
static void physical_keyboard_value(void *ctx, IOReturn result, void *sender, IOHIDValueRef value) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || !physicalKeyboardEnabled) return;
    if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted) {
        pthread_mutex_lock(&kbLock); physicalKeyboardBlocked = 1; physical_keyboard_clear_locked(); pthread_mutex_unlock(&kbLock);
        physical_keyboard_topology();
        physicalKeyboardStatus = @"输入监控权限已失效：物理键盘输入已停止，旧合并输入仍屏蔽；请恢复权限或关闭独立模式。";
        return;
    }
    IOHIDElementRef element = IOHIDValueGetElement(value);
    IOHIDDeviceRef device = IOHIDElementGetDevice(element);
    physical_keyboard_event([NSValue valueWithPointer:device], IOHIDElementGetUsagePage(element),
        IOHIDElementGetUsage(element), IOHIDValueGetIntegerValue(value) != 0);
}
static NSString *physical_keyboard_identity(IOHIDDeviceRef device) {
    id serial = (__bridge id)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDSerialNumberKey));
    id vendor = (__bridge id)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey));
    id product = (__bridge id)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey));
    id transport = (__bridge id)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDTransportKey));
    if ([serial isKindOfClass:NSString.class] && [serial length] && [serial length] <= 512 &&
        [vendor isKindOfClass:NSNumber.class] && [product isKindOfClass:NSNumber.class]) {
        /* Length-delimited fields avoid string concatenation collisions. */
        NSString *bus = [transport isKindOfClass:NSString.class] ? transport : @"";
        return [NSString stringWithFormat:@"serial:%@:%@:%lu:%@:%lu:%@",vendor,product,
            (unsigned long)bus.length,bus,(unsigned long)[serial length],serial];
    }
    /* Do not use location/product-name as persistent identity: another keyboard
       plugged into that USB port must not inherit this player's binding. */
    return [@"connection:" stringByAppendingString:NSUUID.UUID.UUIDString];
}
static void physical_keyboard_attach(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || !physicalKeyboardEnabled) return;
    NSValue *token = [NSValue valueWithPointer:device];
    if (physicalKeyboards[token]) return;
    id product = (__bridge id)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    NSString *name = [product isKindOfClass:NSString.class] ? product : @"物理键盘";
    if (name.length > 400) name = [name substringToIndex:400];
    NSString *key = physical_keyboard_identity(device);
    physicalKeyboards[token] = @{@"id":key, @"name":[NSString stringWithFormat:@"%@ · %@ · %@", name, [NSUUID.UUID.UUIDString substringToIndex:6],
        [key hasPrefix:@"serial:"] ? @"设备序列号" : @"仅本次连接"]};
    physical_keyboard_topology();
}
static void physical_keyboard_remove(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)ctx; (void)result; (void)sender;
    [physicalKeyboards removeObjectForKey:[NSValue valueWithPointer:device]];
    physical_keyboard_topology();
}
int runtime_keyboard_physical_enable(int enabled) {
    kb_load(); physical_keyboard_load();
    if (!!enabled == physicalKeyboardEnabled) return 1;
    if (enabled) {
        if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted) {
            IOHIDRequestAccess(kIOHIDRequestTypeListenEvent); /* explicit UI action only */
            if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted) {
                physicalKeyboardStatus = @"未启用：请在系统设置 → 隐私与安全性 → 输入监控授权后重试（可能需要重启应用）。";
                return 0;
            }
        }
        physicalKeyboardManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (!physicalKeyboardManager) return 0;
        physicalKeyboards = [NSMutableDictionary dictionary];
        IOHIDManagerSetDeviceMatching(physicalKeyboardManager, (__bridge CFDictionaryRef)@{
            @kIOHIDDeviceUsagePageKey:@(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey:@(kHIDUsage_GD_Keyboard)});
        IOHIDManagerRegisterDeviceMatchingCallback(physicalKeyboardManager, physical_keyboard_attach, NULL);
        IOHIDManagerRegisterDeviceRemovalCallback(physicalKeyboardManager, physical_keyboard_remove, NULL);
        IOHIDManagerRegisterInputValueCallback(physicalKeyboardManager, physical_keyboard_value, NULL);
        IOReturn result = IOHIDManagerOpen(physicalKeyboardManager, kIOHIDOptionsTypeNone);
        if (result != kIOReturnSuccess) {
            CFRelease(physicalKeyboardManager); physicalKeyboardManager = NULL;
            physicalKeyboardStatus = @"无法打开物理键盘输入；未改变旧键盘模式。"; return 0;
        }
        pthread_mutex_lock(&kbLock); physicalKeyboardEnabled = 1; physicalKeyboardBlocked = 0; pthread_mutex_unlock(&kbLock);
        IOHIDManagerScheduleWithRunLoop(physicalKeyboardManager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        NSSet *devices = (__bridge_transfer NSSet *)IOHIDManagerCopyDevices(physicalKeyboardManager);
        for (id device in devices) physical_keyboard_attach(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)device);
        physicalKeyboardStatus = @"独立物理键盘已启用：旧合并键盘输入已屏蔽。每个玩家选择一把；设置和失焦时不发送输入。";
        /* Permission revocation can stop callbacks entirely; poll access without
           reading keys so a held input cannot remain stuck indefinitely. */
        physicalKeyboardPermissionTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
        dispatch_source_set_timer(physicalKeyboardPermissionTimer,dispatch_time(DISPATCH_TIME_NOW,500*NSEC_PER_MSEC),500*NSEC_PER_MSEC,50*NSEC_PER_MSEC);
        dispatch_source_set_event_handler(physicalKeyboardPermissionTimer, ^{
            int blocked = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted;
            if (blocked == physicalKeyboardBlocked) return;
            pthread_mutex_lock(&kbLock); physicalKeyboardBlocked = blocked; physical_keyboard_clear_locked(); pthread_mutex_unlock(&kbLock);
            physicalKeyboardStatus = blocked ? @"输入监控权限已失效；物理键盘输入已停止，合并输入仍屏蔽。" : @"输入监控权限已恢复；如未收到键盘输入，请关闭后重新启用独立键盘。";
            physical_keyboard_topology();
        });
        dispatch_resume(physicalKeyboardPermissionTimer);
    } else {
        if (physicalKeyboardPermissionTimer) { dispatch_source_cancel(physicalKeyboardPermissionTimer); physicalKeyboardPermissionTimer = nil; }
        pthread_mutex_lock(&kbLock); physicalKeyboardBlocked = 0; physicalKeyboardEnabled = 0; pthread_mutex_unlock(&kbLock);
        if (physicalKeyboardManager) {
            IOHIDManagerUnscheduleFromRunLoop(physicalKeyboardManager,CFRunLoopGetMain(),kCFRunLoopCommonModes);
            IOHIDManagerClose(physicalKeyboardManager,kIOHIDOptionsTypeNone);
            CFRelease(physicalKeyboardManager); physicalKeyboardManager = NULL;
        }
        [physicalKeyboards removeAllObjects];
        physicalKeyboardStatus = @"独立物理键盘已关闭，恢复原系统键盘模式。";
    }
    runtime_keys_clear(); runtime_keyboard_clear(); physical_keyboard_topology();
    return 1;
}
int runtime_keyboard_physical_enabled(void) { return physicalKeyboardEnabled; } /* main queue */
NSString *runtime_keyboard_physical_status(void) { return physicalKeyboardStatus; }
NSArray *runtime_keyboard_physical_devices(void) {
    physical_keyboard_load();
    NSMutableArray *result = [NSMutableArray array];
    for (NSDictionary *info in physicalKeyboards.allValues)
        if (physical_keyboard_matches(info[@"id"]) == 1) [result addObject:info];
    return [result sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a,NSDictionary *b) { return [a[@"id"] compare:b[@"id"]]; }];
}
NSString *runtime_keyboard_physical_binding(int slot) {
    physical_keyboard_load(); return slot >= 0 && slot < IOS_KEYBOARD_SLOTS ? physicalKeyboardBinding[slot] : nil;
}
NSString *runtime_keyboard_physical_label(int slot) {
    physical_keyboard_load(); if (slot < 0 || slot >= IOS_KEYBOARD_SLOTS) return @"无效玩家";
    if ([physicalKeyboardBinding[slot] isEqual:@"none"]) return @"不绑定物理键盘";
    int matches = physical_keyboard_matches(physicalKeyboardBinding[slot]);
    return [NSString stringWithFormat:@"%@%@",matches == 1 ? @"" : matches ? @"身份重复：" : @"离线：",physicalKeyboardName[slot]];
}
int runtime_keyboard_physical_bind(int slot, NSString *key) {
    physical_keyboard_load();
    if (slot < 0 || slot >= runtime_keyboard_slot_count() || !physical_keyboard_valid_key(key)) return 0;
    NSString *name = @"";
    if (![key isEqual:@"none"]) {
        if (!physicalKeyboardEnabled || physical_keyboard_matches(key) != 1) return 0;
        for (int s = 0; s < IOS_KEYBOARD_SLOTS; ++s)
            if (s != slot && [physicalKeyboardBinding[s] isEqual:key]) return 0;
        for (NSDictionary *info in physicalKeyboards.allValues) if ([info[@"id"] isEqual:key]) name = info[@"name"];
    }
    NSMutableArray *ports = [NSMutableArray array];
    for (int s = 0; s < IOS_KEYBOARD_SLOTS; ++s)
        [ports addObject:@{@"id":s == slot ? key : physicalKeyboardBinding[s], @"name":s == slot ? name : physicalKeyboardName[s]}];
    NSString *path = physical_keyboard_path();
    if (!path || ![@{@"version":@1,@"ports":ports} writeToFile:path atomically:YES]) return 0;
    pthread_mutex_lock(&kbLock);
    physicalKeyboardBinding[slot] = [key copy]; physicalKeyboardName[slot] = name;
    keyboard_state_clear(&physicalKeyboardState[slot]);
    pthread_mutex_unlock(&kbLock);
    physical_keyboard_topology(); return 1;
}
void runtime_keyboard_physical_focus(int focused) {
    pthread_mutex_lock(&kbLock); physicalKeyboardFocused = !!focused; physical_keyboard_clear_locked(); pthread_mutex_unlock(&kbLock);
}
/* Called under kbLock by guest reads. No Foundation/device access here. */
static void physical_keyboard_merge_locked(int slot, RuntimeGamepadState *out) {
    if (!physicalKeyboardEnabled || physicalKeyboardBlocked || !physicalKeyboardConnected[slot]) return;
    out->connected = 1;
    if (!physicalKeyboardFocused || kbSuspended) return;
    KeyboardOutput k = keyboard_state_read(&physicalKeyboardState[slot],&kbMaps[slot]);
    out->buttons |= k.buttons;
    if (abs(k.lx) > abs(out->lx)) out->lx = k.lx;
    if (abs(k.ly) > abs(out->ly)) out->ly = k.ly;
    if (abs(k.rx) > abs(out->rx)) out->rx = k.rx;
    if (abs(k.ry) > abs(out->ry)) out->ry = k.ry;
}
