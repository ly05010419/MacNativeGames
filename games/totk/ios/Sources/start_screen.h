#import <UIKit/UIKit.h>

/* Start screen (2026-10-26): the game's name and icon with its picture choices, kept in the same defaults the Settings
   app writes, and a Start button. Tears of the Kingdom and Breath of the Wild: the picture profile, MetalFX and Advanced
   (picture_settings.h; 2026-11-17). Bananza and Pokopia: TOTK_DOCKED / TOTK_OUTPUT_RES and TOTK_PHONE_FPS.
   Touch or a controller (up / down a row, left / right its value, A or the right face button to start). onStart runs
   once, on the main thread, after the choices are saved; the view removes itself. */
@interface TOTKStartView : UIView
- (instancetype)initWithFrame:(CGRect)frame onStart:(void (^)(void))onStart;
@end
