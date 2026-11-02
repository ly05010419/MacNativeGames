/* Exercise the same preference reader used by the phone services, without sensors or a game. */
#include "../games/totk/ios/Sources/phone_settings.h"
#include <assert.h>

int main(void) {
    @autoreleasepool {
        NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
        NSArray<NSString *> *keys = @[@"TOTK_PHONE_GYRO", @"TOTK_PHONE_HAPTICS"];
        for (NSString *key in keys) unsetenv(key.UTF8String);
        [defaults setVolatileDomain:@{} forName:NSArgumentDomain];
        for (NSString *key in keys) assert(totk_phone_setting_enabled(key));  // Settings never opened

        for (NSString *key in keys) {
            NSString *other = keys[[key isEqualToString:keys[0]] ? 1 : 0];
            [defaults setVolatileDomain:@{key: @NO} forName:NSArgumentDomain];
            assert(!totk_phone_setting_enabled(key));
            assert(totk_phone_setting_enabled(other));  // the switches are independent
            setenv(key.UTF8String, "1", 1);
            assert(totk_phone_setting_enabled(key));    // devicectl overrides Settings off
            unsetenv(key.UTF8String);
            assert(!totk_phone_setting_enabled(key));
            [defaults setVolatileDomain:@{key: @YES} forName:NSArgumentDomain];
            assert(totk_phone_setting_enabled(key));
            setenv(key.UTF8String, "0", 1);
            assert(!totk_phone_setting_enabled(key));   // devicectl overrides Settings on
            unsetenv(key.UTF8String);
        }
    }
    return 0;
}
