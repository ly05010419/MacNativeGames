#import <UIKit/UIKit.h>

/* Advanced (user 2026-11-17): every control of the picture profiles, opened from the
   start screen of Tears of the Kingdom and Breath of the Wild. It shows the chosen profile's values; changing any
   control but MetalFX saves them as Custom (picture_settings.h); MetalFX is its own setting. Each control has a
   line saying what its choice does. Touch or a controller (up / down a row, left / right its value, A on Play, B back).
   onBack and onStart run on the main thread; the view does not remove itself. */
@interface TOTKAdvancedView : UIView
- (instancetype)initWithFrame:(CGRect)frame onBack:(void (^)(void))onBack onStart:(void (^)(void))onStart;
@end
