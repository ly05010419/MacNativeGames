#import <Foundation/Foundation.h>
#include "totk_contrib_upload.h"

NSString *other_choice(NSString *path) { return totk_inbox_choice(path); }
NSUInteger other_send(NSString *path) { return totk_inbox_send(path, @"mac"); }
void *other_gate_address(void) { return &totk_inbox_session_off; }
void *other_timer_address(void) { return &totk_inbox_timer; }
