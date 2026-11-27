#pragma once
#import <Foundation/Foundation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

/* A modal panel must not occupy a main-dispatch-queue work item: controller
   notifications also use that serial queue. Run the panel from the run loop
   instead, leaving that queue available throughout the nested modal loop.
   The guest caller releases its virtual core before waiting here. */
static inline void lm3_controller_perform_main_and_wait(void (^work)(void)) {
    if (NSThread.isMainThread) { work(); return; }
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    CFRunLoopRef main = CFRunLoopGetMain();
    CFRunLoopPerformBlock(main, kCFRunLoopCommonModes, ^{
        @autoreleasepool { work(); }
        dispatch_semaphore_signal(finished);
    });
    CFRunLoopWakeUp(main);
    dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);
}
