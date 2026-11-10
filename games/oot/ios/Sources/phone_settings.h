/* Settings.bundle's phone switches are read once when each service starts. The Settings app need not have been opened:
   a missing preference means on. An explicit devicectl environment variable wins, as for the other iOS settings. */
#ifndef TOTK_PHONE_SETTINGS_H
#define TOTK_PHONE_SETTINGS_H
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <string.h>

static inline BOOL totk_phone_setting_enabled(NSString *key) {
    const char *env = getenv(key.UTF8String);
    if (env) return strcmp(env, "0") != 0;
    id saved = [NSUserDefaults.standardUserDefaults objectForKey:key];
    return saved ? [saved boolValue] : YES;
}
#endif
