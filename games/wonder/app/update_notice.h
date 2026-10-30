#pragma once
#import <AppKit/AppKit.h>
#include "update_check.h"
#include "../third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h"

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

// The launcher reads only a previous result. The runtime owns the background task:
// a dispatch block started here would be destroyed by the launcher's execv().
static void mng_update_notice(NSString *dataFolder) {
    unsetenv("MNG_UPDATE_REVISION");
    unsetenv("MNG_UPDATE_DATA");
    if (!mng_updates_enabled()) return;
    NSString *base = [NSBundle.mainBundle objectForInfoDictionaryKey:@"MNGSourceRevision"];
    if (!mng_revision(base)) return;
    setenv("MNG_UPDATE_REVISION", base.UTF8String, 1);
    setenv("MNG_UPDATE_DATA", dataFolder.fileSystemRepresentation, 1);
    NSDictionary *state = [NSDictionary dictionaryWithContentsOfFile:mng_update_state_path(dataFolder)] ?: @{};
    NSString *head = mng_cached_update(state, base, mng_update_skipped(dataFolder));
    if (!head) return;
    NSModalResponse choice = mng_update_alert(base, head);
    if (choice == NSAlertThirdButtonReturn) {
        // Separate from the worker's cache so an in-flight check cannot undo Skip.
        [head writeToFile:mng_update_skip_path(dataFolder) atomically:YES encoding:NSUTF8StringEncoding error:nil];
    } else if (choice == NSAlertSecondButtonReturn) {
        // Never execute or follow URLs supplied in a GitHub response.
        NSString *url = [NSString stringWithFormat:@"https://github.com/ly05010419/MacNativeGames/compare/%@...%@", base, head];
        [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:url]];
    }
}
