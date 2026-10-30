/* runtime/icons_apply.m without the game: the layout is the default, the Python is tests/test_icons_apply.py's fake.
   argv[1]: run (start and wait), second (a second start while running), cancel (cancel once the drawing runs), stale,
   early (start, then at once read the state and cancel while the tools check still runs: how long each took, in ms). */
#import <Foundation/Foundation.h>
#include "../games/totk/runtime/icons_apply.h"
#include "../games/totk/runtime/keyboard_map.h"

void runtime_keymap_get(KeyMap *out) { keymap_default(out); }

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) return 2;
        NSString *mode = @(argv[1]);
        if ([mode isEqual:@"stale"]) { printf("stale %d\n", icons_apply_stale()); return 0; }
        if ([mode isEqual:@"early"]) {
            icons_apply_start(^(IconsApplyState state, int step, int steps, NSString *detail) {
                printf("update %d %d/%d %s\n", (int)state, step, steps, detail.UTF8String ?: ""); fflush(stdout);
            });
            NSDate *t = NSDate.date; icons_apply_state(NULL, NULL);
            printf("state ms %d\n", (int)(-t.timeIntervalSinceNow * 1000));
            t = NSDate.date; icons_apply_cancel();
            printf("cancel ms %d\n", (int)(-t.timeIntervalSinceNow * 1000));
            printf("after cancel %d\n", (int)icons_apply_state(NULL, NULL)); fflush(stdout);
            [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:4]];
            return 0;
        }
        __block BOOL over = NO, asked = NO;
        BOOL started = icons_apply_start(^(IconsApplyState state, int step, int steps, NSString *detail) {
            printf("update %d %d/%d %s\n", (int)state, step, steps, detail.UTF8String ?: "");
            if (state == IconsApplyRunning && step >= 3 && !asked) {
                asked = YES;
                if ([mode isEqual:@"second"]) printf("second %d\n", (int)icons_apply_start(nil));
                if ([mode isEqual:@"cancel"]) { icons_apply_cancel(); printf("cancelled %d\n", (int)icons_apply_state(NULL, NULL)); over = YES; }
            }
            if (state != IconsApplyRunning) over = YES;
            fflush(stdout);
        });
        printf("started %d\n", (int)started); fflush(stdout);
        NSDate *limit = [NSDate dateWithTimeIntervalSinceNow:30];
        while (!over && [limit timeIntervalSinceNow] > 0)
            [NSRunLoop.mainRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        printf("over %d\n", (int)over);
    }
    return 0;
}
