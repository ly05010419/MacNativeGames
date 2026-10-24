#pragma once
#import <AppKit/AppKit.h>
#include "../third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h"

// Public GitHub metadata only; never downloads or installs an update.
static BOOL mng_revision(id value) {
    return [value isKindOfClass:NSString.class] && [value length] == 40 &&
        [value rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"].invertedSet].location == NSNotFound;
}

static BOOL mng_relevant_path(id value) {
    if (![value isKindOfClass:NSString.class]) return NO;
    NSString *path = value;
    if ([@[@"md", @"rst"] containsObject:path.pathExtension.lowercaseString]) return NO;
    if ([path hasPrefix:@"games/totk/ios/"] || [path hasPrefix:@"games/totk/tests/"]) return NO;
    return [path hasPrefix:@"games/totk/"] || [path hasPrefix:@"vendor/"] ||
        [path isEqualToString:@"build_app.py"] || [path isEqualToString:@"Makefile"];
}

static BOOL mng_comparison_has_update(id value) {
    if (![value isKindOfClass:NSDictionary.class]) return NO;
    NSDictionary *d = value;
    if (!([d[@"status"] isEqual:@"ahead"] || [d[@"status"] isEqual:@"diverged"]) || ![d[@"ahead_by"] isKindOfClass:NSNumber.class] ||
        [d[@"ahead_by"] integerValue] <= 0 || ![d[@"files"] isKindOfClass:NSArray.class]) return NO;
    for (id f in d[@"files"]) {
        if (![f isKindOfClass:NSDictionary.class]) continue;
        if (mng_relevant_path(f[@"filename"]) || mng_relevant_path(f[@"previous_filename"])) return YES;
    }
    // GitHub truncates file lists at 300. Avoid silently missing changes beyond that limit.
    return [d[@"files"] count] >= 300;
}

static NSDictionary *mng_public_json(NSString *suffix, double deadline) {
    double remaining = deadline - NSProcessInfo.processInfo.systemUptime;
    if (remaining <= 0) return nil;
    NSURL *url = [NSURL URLWithString:[@"https://api.github.com/repos/ly05010419/MacNativeGames/" stringByAppendingString:suffix]];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:remaining];
    [request setValue:@"MacNativeGames-update-notice" forHTTPHeaderField:@"User-Agent"];
    [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
    NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.timeoutIntervalForResource = remaining;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:config];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NSDictionary *result = nil;
    NSURLSessionDataTask *task = [session dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        if (!error && [response isKindOfClass:NSHTTPURLResponse.class] &&
            [(NSHTTPURLResponse *)response statusCode] == 200 && data.length <= 16 * 1024 * 1024) {
            id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            if ([json isKindOfClass:NSDictionary.class]) result = json;
        }
        dispatch_semaphore_signal(done);
    }];
    [task resume];
    BOOL finished = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(remaining * NSEC_PER_SEC))) == 0;
    [session invalidateAndCancel];
    return finished ? result : nil;
}

static NSModalResponse mng_update_alert(NSString *base, NSString *head) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    [NSApp activateIgnoringOtherApps:YES];
    NSAlert *alert = [NSAlert new];
    alert.messageText = @(TOTK_UI("MacNativeGames 移植有更新", "MacNativeGames port update available"));
    alert.informativeText = [NSString stringWithFormat:@(TOTK_UI(
        "GitHub 主分支有此构建之后的更改（%@ → %@）。\n\n可以查看更改，然后手动重建移植。新构建的兼容性尚需确认。\n\n游戏将使用当前版本启动，不会自动下载或安装更新。",
        "The GitHub main branch has changes since this build (%@ → %@).\n\nYou can review the changes and rebuild the port manually. Compatibility of a new build still needs checking.\n\nThe game will start with its current version. No update is downloaded or installed.")),
        [base substringToIndex:7], [head substringToIndex:7]];
    [alert addButtonWithTitle:@(TOTK_UI("开始游戏", "Play"))];
    [alert addButtonWithTitle:@(TOTK_UI("查看更改", "View changes"))];
    [alert addButtonWithTitle:@(TOTK_UI("跳过此更新", "Skip this update"))];
    return [alert runModal];
}

static void mng_update_notice(NSString *dataFolder) {
    const char *enabled = getenv("MNG_CHECK_UPDATES");
    if (enabled && !strcmp(enabled, "0")) return;
    NSString *base = [NSBundle.mainBundle objectForInfoDictionaryKey:@"MNGSourceRevision"];
    if (!mng_revision(base)) return;
    NSString *path = [dataFolder stringByAppendingPathComponent:@"port-update-state.plist"];
    NSDictionary *old = [NSDictionary dictionaryWithContentsOfFile:path] ?: @{};
    NSMutableDictionary *state = [old mutableCopy];
    double deadline = NSProcessInfo.processInfo.systemUptime + 3.0;
    NSDictionary *ref = mng_public_json(@"git/ref/heads/main", deadline);
    id object = ref[@"object"];
    NSString *head = [object isKindOfClass:NSDictionary.class] ? object[@"sha"] : nil;
    if (!mng_revision(head) || [head isEqual:base] || [head isEqual:state[@"skipped"]]) return;
    NSString *comparison = [NSString stringWithFormat:@"compare/%@...%@?per_page=1", base, head];
    if (!mng_comparison_has_update(mng_public_json(comparison, deadline))) return;
    NSModalResponse choice = mng_update_alert(base, head);
    if (choice == NSAlertThirdButtonReturn) {
        state[@"skipped"] = head;
        [state writeToFile:path atomically:YES];
    } else if (choice == NSAlertSecondButtonReturn) {
        // Build the URL ourselves: never execute or follow URLs supplied in a GitHub response.
        NSString *url = [NSString stringWithFormat:@"https://github.com/ly05010419/MacNativeGames/compare/%@...%@", base, head];
        [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:url]];
    }
}
