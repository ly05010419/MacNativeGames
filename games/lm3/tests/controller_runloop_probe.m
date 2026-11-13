/* No NSApplication, window or controller discovery: reproduce the serial-main
   queue starvation inside a modal-mode run loop, using the production helper. */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#include "lm3_controller_dispatch.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    _Atomic int gcd, operation, timer, phase, done;
    int nested_gcd, nested_operation, nested_timer, on_main;
    double elapsed;
} ProbeResult;
static ProbeResult result;

static void nested_mode(void) {
    result.on_main=NSThread.isMainThread;
    atomic_store(&result.phase,1);
    double start=CFAbsoluteTimeGetCurrent();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_MSEC),
                   dispatch_get_global_queue(QOS_CLASS_DEFAULT,0), ^{
        dispatch_async(dispatch_get_main_queue(), ^{ atomic_fetch_add(&result.gcd,1); });
        [NSOperationQueue.mainQueue addOperationWithBlock:^{ atomic_fetch_add(&result.operation,1); }];
    });
    NSTimer *timer=[NSTimer timerWithTimeInterval:0.04 repeats:NO block:^(NSTimer *t){
        (void)t;atomic_fetch_add(&result.timer,1);
    }];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSModalPanelRunLoopMode];
    while(CFAbsoluteTimeGetCurrent()-start<0.25) {
        CFRunLoopRunInMode((__bridge CFStringRef)NSModalPanelRunLoopMode,0.01,true);
        if(CFAbsoluteTimeGetCurrent()-start>=0.08&&atomic_load(&result.gcd)&&atomic_load(&result.operation))break;
    }
    result.nested_gcd=atomic_load(&result.gcd);result.nested_operation=atomic_load(&result.operation);
    result.nested_timer=atomic_load(&result.timer);result.elapsed=CFAbsoluteTimeGetCurrent()-start;
    [timer invalidate];atomic_store(&result.phase,2);
}

int main(int argc,const char **argv) { @autoreleasepool {
    if(argc==2&&!strcmp(argv[1],"main")) {
        __block int called=0,on_main=0;
        lm3_controller_perform_main_and_wait(^{called=1;on_main=NSThread.isMainThread;});
        printf("{\"main_fast_path\":%d,\"on_main\":%d}\n",called,on_main);
        return called&&on_main?0:1;
    }
    int old=argc==2&&!strcmp(argv[1],"old-dispatch-sync");
    CFRunLoopAddCommonMode(CFRunLoopGetMain(),(__bridge CFStringRef)NSModalPanelRunLoopMode);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0), ^{
        if(old)dispatch_sync(dispatch_get_main_queue(),^{nested_mode();});
        else lm3_controller_perform_main_and_wait(^{nested_mode();});
        /* The synchronous SDK caller must resume only after its UI work ends. */
        atomic_store(&result.done,atomic_load(&result.phase)==2?1:-1);
    });
    double start=CFAbsoluteTimeGetCurrent();
    while(!atomic_load(&result.done)&&CFAbsoluteTimeGetCurrent()-start<0.80)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.01,true);
    int passed=atomic_load(&result.done)==1&&result.on_main&&result.nested_gcd==1&&
        result.nested_operation==1&&result.nested_timer==1;
    printf("{\"status\":\"%s\",\"nested_gcd\":%d,\"nested_operation\":%d,\"nested_timer\":%d,\"wait_completed\":%d,\"on_main\":%d,\"elapsed\":%.3f}\n",
        passed?"pass":"fail",result.nested_gcd,result.nested_operation,result.nested_timer,
        atomic_load(&result.done),result.on_main,result.elapsed);
    if(!passed)fprintf(stderr,"modal-loop main-queue delivery failed\n");
    return passed?0:1;
}}
