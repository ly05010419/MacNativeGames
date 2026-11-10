#import <UIKit/UIKit.h>

/* Start screen (2026-10-26): the game's name and icon with its picture choices, kept in the same defaults the Settings
   app writes (TOTK_DOCKED: Full HD docked, shown only when the game's Settings pane has it; TOTK_PHONE_FPS: "30" or "60"),
   and a Start button.
   Touch or a controller (up / down a row, left / right its value, A or the right face button to start). onStart runs
   once, on the main thread, after the choices are saved; the view removes itself. */
@interface TOTKStartView : UIView
- (instancetype)initWithFrame:(CGRect)frame onStart:(void (^)(void))onStart;
@end
