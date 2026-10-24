/* The iPhone's / iPad's own motion sensors as the controller's six-axis sensor (device_motion.m). Main thread. */
#import <UIKit/UIKit.h>
void totk_device_motion_set_orientation(UIInterfaceOrientation o);
void totk_device_motion_begin(void);          /* once, when the game starts; nothing unless runtime_gyro_enabled() */
void totk_device_motion_pause(BOOL paused);   /* app resigned active (YES) / became active again (NO) */
void totk_device_motion_note_touch(void);     /* the touch layer saw a finger: the phone's gyro takes the aiming (main thread) */
double totk_device_motion_sensitivity(void);            /* the player's gain, 0.1-2 (Documents/save/motion.json) */
void totk_device_motion_set_sensitivity(double percent); /* 10-200: saves it and applies it to the running sensor */
