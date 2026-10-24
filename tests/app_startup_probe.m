/* Exercise the production startup preparation and resource lookup without game data or a GPU. */
#import <Foundation/Foundation.h>
#include <unistd.h>
#include "totk_app_resources.h"

int runtime_app_launch(int argc, char **argv, int (*run_game)(int, char **));
/* Isolate the production startup's user-data and log paths without adding a production setting. */
NSString *NSHomeDirectory(void) { return @(getenv("MNG_TEST_HOME")); }
static int calls;
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc == 3) {
            NSDictionary *report = @{
                @"calls": @(++calls), @"pid": @(getpid()),
                @"executable": @(argv[0]), @"modules": @(argv[1]), @"mode": @(argv[2]),
                @"bundle": NSBundle.mainBundle.bundleIdentifier ?: @"",
                @"resources": totk_app_shader_build_dir() ?: @"",
                @"cwd": NSFileManager.defaultManager.currentDirectoryPath,
                @"profile": @(getenv("TOTK_PROFILE_DIR") ?: ""),
                @"cache": @(getenv("TOTK_SHADER_CACHE") ?: ""),
                @"input": @(getenv("TOTK_INPUT_FILE") ?: "")
            };
            NSData *json = [NSJSONSerialization dataWithJSONObject:report options:0 error:NULL];
            return [json writeToFile:@(getenv("MNG_STARTUP_REPORT")) atomically:YES] ? 0 : 2;
        }
        return runtime_app_launch(argc, argv, main);
    }
}
