/* Configuration on main queue; guest and event paths read atomically. */
#include "input_source.h"
static _Atomic int inputSource[RUNTIME_MAX_GAMEPADS] = {[0] = INPUT_SOURCE_COMPAT,
#if RUNTIME_MAX_GAMEPADS > 1
    [1 ... RUNTIME_MAX_GAMEPADS - 1] = INPUT_SOURCE_OFF
#endif
};
static int inputSourcesLoaded, inputSourcesConfigured;
int runtime_input_source(int slot) {
    return slot >= 0 && slot < RUNTIME_MAX_GAMEPADS ? atomic_load(&inputSource[slot]) : -1;
}
int runtime_input_source_allows(int slot, unsigned device) {
    return (input_source_mask(runtime_input_source(slot)) & device) != 0;
}
static NSString *input_source_path(void) {
    char path[4096];
    return setting_path("input-sources.plist", path, sizeof path) ? @(path) : nil;
}
static int input_source_supported(int slot, int source) {
    if (slot < 0 || slot >= RUNTIME_MAX_GAMEPADS || source < 0 || source >= INPUT_SOURCE_COUNT) return 0;
    if (source == INPUT_SOURCE_TOUCH) return TARGET_OS_IPHONE && slot == 0;
    if (source == INPUT_SOURCE_KEYBOARD_MOUSE) return TARGET_OS_OSX && slot == 0;
    return 1;
}
static void input_source_load(void) {
    if (inputSourcesLoaded) return;
    inputSourcesLoaded = 1;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s)
        atomic_store(&inputSource[s], s ? INPUT_SOURCE_OFF : INPUT_SOURCE_COMPAT);
    NSString *path = input_source_path();
    NSDictionary *root = path ? [NSDictionary dictionaryWithContentsOfFile:path] : nil;
    if (![root isKindOfClass:NSDictionary.class] || ![root[@"version"] isEqual:@1]) return;
    NSArray *sources = root[@"sources"];
    if (![sources isKindOfClass:NSArray.class] || sources.count != RUNTIME_MAX_GAMEPADS) return;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) {
        NSNumber *value = sources[s];
        if (![value isKindOfClass:NSNumber.class] || value.doubleValue != value.intValue ||
            !input_source_supported(s, value.intValue)) return;
    }
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) atomic_store(&inputSource[s], [sources[s] intValue]);
    inputSourcesConfigured = 1;
}
int runtime_input_source_set(int slot, int source) {
    input_source_load();
    if (!input_source_supported(slot, source)) return 0;
    NSMutableArray *sources = [NSMutableArray array];
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) [sources addObject:@(s == slot ? source : runtime_input_source(s))];
    NSString *path = input_source_path();
    if (!path || ![@{@"version":@1,@"sources":sources} writeToFile:path atomically:YES]) return 0;
    atomic_store(&inputSource[slot],source);
    inputSourcesConfigured = 1;
    /* Clear all merged/pending paths at this transition. Mappings and bindings
       are retained, so switching back never silently restores stale presses. */
    runtime_input_clear(); runtime_mouse_release();
    if (slot == 0) runtime_mouse_refresh_source();
    if (started) refresh_gamepads();
    runtime_input_gamepads_changed();
    return 1;
}
