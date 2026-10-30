/* The shared shader list contribution sent with the GitHub CLI (gh): the Mac launcher's reminder ("Submit automatically",
   app/launcher.m) and the settings panel's button (runtime/settings_panel.h). Mac only (NSTask). Uses totk_shader_contrib.h
   and game_version.h. */
#pragma once
#import <Foundation/Foundation.h>

/* A tool's standard output and exit status (input, if any, goes to its standard input); nil if it did not start. */
static inline NSString *run_tool(NSString *exe, NSArray<NSString *> *args, NSString *input, int *status) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:exe]; task.arguments = args;
    NSPipe *out = [NSPipe pipe], *in = [NSPipe pipe];
    task.standardOutput = out; task.standardInput = in; task.standardError = [NSFileHandle fileHandleWithNullDevice];
    *status = -1;
    if (![task launchAndReturnError:nil]) return nil;
    if (input) [in.fileHandleForWriting writeData:[input dataUsingEncoding:NSUTF8StringEncoding]];
    [in.fileHandleForWriting closeFile];
    NSData *data = [out.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit]; *status = task.terminationStatus;
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

/* The GitHub CLI when it is installed and signed in, else nil (an app started from Finder has no Homebrew in its PATH).
   TOTK_CONTRIB_GH=0 (a test of the path without gh, e.g. open --env TOTK_CONTRIB_GH=0 <app>): as if it were not there. */
static inline NSString *gh_path(void) {
    const char *off = getenv("TOTK_CONTRIB_GH");
    if (off && !strcmp(off, "0")) return nil;
    NSMutableArray<NSString *> *candidates = [NSMutableArray array];
    for (NSString *dir in [NSProcessInfo.processInfo.environment[@"PATH"] componentsSeparatedByString:@":"])
        if (dir.length) [candidates addObject:[dir stringByAppendingPathComponent:@"gh"]];
    [candidates addObjectsFromArray:@[@"/opt/homebrew/bin/gh", @"/usr/local/bin/gh"]];
    for (NSString *exe in candidates) {
        if (![NSFileManager.defaultManager isExecutableFileAtPath:exe]) continue;
        int status; run_tool(exe, @[@"auth", @"status"], nil, &status);
        return status == 0 ? exe : nil;
    }
    return nil;
}

/* "Submit automatically": a secret gist with the file, then an issue in the shader-list form's layout linking its raw
   file (scripts/collect_shader_issues.py finds it by its title; GitHub drops labels set by people without write access).
   The issue's link, or nil. */
static inline NSString *contrib_submit(NSString *gh, NSString *path, NSUInteger rows) {
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!text) return nil;
    int status;
    NSString *gist = run_tool(gh, @[@"gist", @"create", @"--filename", @"shader-list-contribution.txt", @"--desc", @"MacNativeGames shader list contribution", @"-"], text, &status);
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"https://gist\\.github\\.com/([\\w-]+)/([0-9a-f]+)" options:0 error:nil];
    NSTextCheckingResult *m = status == 0 && gist ? [re firstMatchInString:gist options:0 range:NSMakeRange(0, gist.length)] : nil;
    if (!m) return nil;
    NSString *link = [NSString stringWithFormat:@"https://gist.githubusercontent.com/%@/%@/raw/shader-list-contribution.txt",
                      [gist substringWithRange:[m rangeAtIndex:1]], [gist substringWithRange:[m rangeAtIndex:2]]];
    NSString *body = [NSString stringWithFormat:@"### Game version / 游戏版本\n\n%s\n\n### Device / 设备\n\nMac\n\n### shader-list-contribution.txt\n\n%@\n\n"
                      "### Where you played (optional) / 玩了哪些地方（可选）\n\n_No response_\n", TOTK_GAME_VERSION, link];
    NSString *title = [NSString stringWithFormat:@GAME_CONTRIB_TITLE " %lu shaders from Mac (%@ %s)", (unsigned long)rows, @GAME_FOLDER, TOTK_GAME_VERSION];
    NSString *issue = run_tool(gh, @[@"issue", @"create", @"--repo", @TOTK_CONTRIB_REPO, @"--title", title, @"--body", body], nil, &status);
    NSRange r = status == 0 && issue ? [issue rangeOfString:@"https://github\\.com/\\S+/issues/[0-9]+" options:NSRegularExpressionSearch] : NSMakeRange(NSNotFound, 0);
    return r.location == NSNotFound ? nil : [issue substringWithRange:r];
}
