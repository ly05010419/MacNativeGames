#import <UIKit/UIKit.h>

/* On-screen controller for iPhone / iPad, laid out like a Switch controller so the game's own button prompts match. It
   reports its state through runtime_touch_pad_set (runtime/gamepad.m), which merges it with any physical controller. */
@interface DreadTouchControls : UIView
@end
