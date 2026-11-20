/* The few front-end functions runtime/frontend.m gives the Mac that the iPhone / iPad app answers itself
   (runtime/frontend_ios.m has the rest): the screen's refresh rate for the frame-rate choice, and the Mac settings
   panel's entry points, which have no panel on the phone (the Settings app pane is ios/Settings.bundle). */
#import <UIKit/UIKit.h>
#include "runtime.h"

int runtime_display_max_hz(void) {
    __block NSInteger hz = 0;
    void (^read)(void) = ^{
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
            if ([scene isKindOfClass:UIWindowScene.class]) hz = MAX(hz, ((UIWindowScene *)scene).screen.maximumFramesPerSecond);
    };
    if (NSThread.isMainThread) read(); else dispatch_sync(dispatch_get_main_queue(), read);
    return (int)hz;
}

void runtime_settings_toggle(void) {}
void runtime_settings_shot(int page, const char *path) { (void)page; (void)path; }
