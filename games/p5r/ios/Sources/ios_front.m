/* Persona 5 Royal's side of the shared iPhone / iPad app (games/totk/ios/Sources, which
   games/p5r/ios/tools/build_ipa.py compiles with GAME_P5R, as Mario Kart 8 Deluxe's, whose runtime this one is): what
   runtime/frontend.m gives the Mac and the shared shell asks of a game runtime, answered here (the shared
   runtime/frontend_ios.m has the rest, with the sized layer, crop and exit calls of vi this game makes). */
#import <UIKit/UIKit.h>
#include <stdio.h>
#include "runtime.h"

/* The fastest refresh rate of the screen (the iOS display link runs at 60; the game presents every second one). */
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

/* The phone's frame rate as Tears of the Kingdom's touch controls and picture settings know it (its hle_nvn.c): the game
   this file was made for (not yet gone over for Persona 5 Royal, whose iPhone app is not made) runs at its own 30 (it presents with interval 2, as on the Mac: frame time p50 33.3 ms); the touch controls
   show no button for it (touch_pad.m, port_features.h SHELL_FPS_CHOICE). */
int runtime_phone_fps(void) { return 30; }
void runtime_phone_fps_set(int fps) { (void)fps; }
