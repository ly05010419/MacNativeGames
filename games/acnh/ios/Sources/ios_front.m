/* Animal Crossing's side of the shared iPhone / iPad app (games/totk/ios/Sources, which games/acnh/ios/tools/build_ipa.py
   compiles with GAME_ACNH): what runtime/frontend.m gives the Mac and the shared shell asks of a game runtime, answered
   here (runtime/frontend_ios.m has the rest). */
#import <UIKit/UIKit.h>
#include <stdio.h>
#include "runtime.h"

/* The fastest refresh rate of the screen (frame_rate.c: 120 FPS needs a 120 Hz screen; the iOS display link runs at 60). */
int runtime_display_max_hz(void) {
    __block NSInteger hz = 0;
    void (^read)(void) = ^{
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
            if ([scene isKindOfClass:UIWindowScene.class]) hz = MAX(hz, ((UIWindowScene *)scene).screen.maximumFramesPerSecond);
    };
    if (NSThread.isMainThread) read(); else dispatch_sync(dispatch_get_main_queue(), read);
    return (int)hz;
}

/* The Mac settings panel's screenshot test (input.c "shot"): no panel on the phone. */
void runtime_settings_shot(int page, const char *path) { (void)page; (void)path; }

/* The phone's frame rate as Tears of the Kingdom's touch controls and picture settings know it (its hle_nvn.c): here the
   rate is the one the game started with (frame_rate.c, ACNH_FPS from the picture settings), and it cannot change while
   playing, since the 60 FPS text is mapped at start (loader.c). The touch controls show no button for it (touch_pad.m). */
int runtime_phone_fps(void) { return runtime_frame_rate_active() == 60 ? 60 : 30; }
void runtime_phone_fps_set(int fps) { (void)fps; }
