/* The phone gyro's hand-over API, kept by the iPhone / iPad motion aiming (phone_motion.m).
   Main thread unless noted. The Settings switch and the app wiring stay phone_motion's own (TOTK_PHONE_GYRO). */
#import <Foundation/Foundation.h>

void totk_device_motion_note_touch(void);      /* the touch layer saw a finger: the phone's gyro takes the aiming over */
