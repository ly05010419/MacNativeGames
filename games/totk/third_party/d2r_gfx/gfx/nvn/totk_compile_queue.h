/* Streaming compilation follows Apple's "Explore Metal 4 games" (WWDC25):
   synchronous compiler calls on DEFAULT-QoS workers, bounded by the device's
   maximumConcurrentCompilationTaskCount. The submit thread never waits here.
   Existing descriptor snapshots, cache locks and submit-thread adoption remain
   the owners of their state. Used only for optional background work. */
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static inline NSOperationQueue *totk_compile_queue(id<MTLDevice> device) {
    static NSOperationQueue *queue;
    static dispatch_queue_t underlying;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSUInteger count = [device respondsToSelector:@selector(maximumConcurrentCompilationTaskCount)]
            ? device.maximumConcurrentCompilationTaskCount : 2;
        queue = [NSOperationQueue new];
        queue.name = @"totk.metal.streaming-compile";
        queue.qualityOfService = NSQualityOfServiceDefault;
        underlying = dispatch_queue_create("totk.metal.streaming-compile",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_DEFAULT, 0));
        queue.underlyingQueue = underlying;   // NSOperationQueue does not retain this property
        queue.maxConcurrentOperationCount = (NSInteger)MAX((NSUInteger)1, count);
    });
    return queue;
}
static inline void totk_compile_submit(id<MTLDevice> device, void (^build)(void)) {
    [totk_compile_queue(device) addOperationWithBlock:^{ @autoreleasepool { build(); } }];
}
/* Only a background translator may wait here: its job cannot publish a shader
   as ready until its Metal library is complete. Never call from this pool or
   the render/submit thread. Urgent foreground dependencies keep their own QoS. */
static inline void totk_compile_from_translator(id<MTLDevice> device, void (^build)(void)) {
    NSBlockOperation *operation = [NSBlockOperation blockOperationWithBlock:^{ @autoreleasepool { build(); } }];
    operation.qualityOfService = NSQualityOfServiceDefault;
    [totk_compile_queue(device) addOperations:@[operation] waitUntilFinished:YES];
}
