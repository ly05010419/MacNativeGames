/* Shader resources belong to the app's Resources tree, independently of where its main executable lives.
   Standalone tools retain their executable-relative lookup; iOS keeps its existing flat bundle layout. */
#pragma once
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

static inline NSString *totk_app_shader_build_dir(void) {
#if TARGET_OS_IPHONE
    return nil;
#else
    NSBundle *bundle = NSBundle.mainBundle;
    if (![[bundle objectForInfoDictionaryKey:@"CFBundlePackageType"] isEqual:@"APPL"]) return nil;
    NSString *path = [bundle.resourcePath stringByAppendingPathComponent:@"totk/build"];
    BOOL directory = NO;
    return [NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] && directory ? path : nil;
#endif
}
