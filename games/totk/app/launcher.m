// The launcher inside the packaged app (build_app.py compiles it as Contents/MacOS/TOTK).
// The game itself is Contents/Resources/totk/build/totk-native; everything it writes goes to the player's own folders:
//   ~/Library/Application Support/MacNativeGames/TOTK/   profile (account, saves), shader cache, game slot, RomFS proof (BotW/ for Breath of the Wild)
//   ~/Library/Logs/MacNativeGames/totk.log             this run, only with the debug log on (totk.previous.log: the one before; botw.log for Breath of the Wild)
// If the game is already running, the launcher only brings it to the front.
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/proc.h>
#include <unistd.h>
#include "dmsh_chain.h"   // DMSH_CHAIN: the shader translation chain the cache folder is named after
#include "game_version.h"
#include "controller_map.h"
#include "include/totk_ui_lang.h"
#include "totk_shader_contrib.h"   // the shared shader list contribution and its reminder
#include "contrib_submit.h"        // its "Submit automatically" (gh), shared with the settings panel
#include "update_notice.h"

static void show_error(NSString *detail) {
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"The game could not start";
    alert.informativeText = detail;
    [alert runModal];
}

/* Whether a game controller is connected: a HID joystick / gamepad (DualSense, Pro Controller, Xbox over Bluetooth),
   or one the GameController framework adopts (Xbox over USB), which it reports a moment after the first run loop turns. */
static BOOL controller_connected(void) {
    IOHIDManagerRef hid = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    NSMutableArray *matching = [NSMutableArray array];
    for (NSNumber *usage in @[@(kHIDUsage_GD_Joystick), @(kHIDUsage_GD_GamePad), @(kHIDUsage_GD_MultiAxisController)])
        [matching addObject:@{@kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: usage}];
    IOHIDManagerSetDeviceMatchingMultiple(hid, (__bridge CFArrayRef)matching);
    CFSetRef devices = IOHIDManagerCopyDevices(hid);
    BOOL found = devices && CFSetGetCount(devices) > 0;
    if (devices) CFRelease(devices);
    CFRelease(hid);
    for (int i = 0; !found && i < 10; i++) {
        for (GCController *controller in GCController.controllers)
            if (controller.extendedGamepad) found = YES;
        if (!found) [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    return found;
}

static NSDictionary<NSString *, NSString *> *kv_read(NSString *path) {
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *key = [line substringToIndex:eq.location];
        if (!out[key]) out[key] = [line substringFromIndex:eq.location + 1];
    }
    return out;
}

static uint64_t pid_start(pid_t pid) {   // 0 if gone or a zombie; with the pid it names one process exactly
    struct proc_bsdinfo bi;
    if (pid <= 0 || proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi || bi.pbi_status == SZOMB) return 0;
    return bi.pbi_start_tvsec;
}

/* The game slot (runtime/game_slot.c): the running game holds an exclusive lock on <slot>/lock and names itself in
   <slot>/holder. Returns the running game's pid, or 0. */
static pid_t running_game(NSString *slot) {
    int fd = open([slot stringByAppendingPathComponent:@"lock"].fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    if (!held) return 0;
    NSDictionary *h = kv_read([slot stringByAppendingPathComponent:@"holder"]);
    /* A player launch must reach runtime/game_slot.c to preempt an agent's
       test. Only an existing player game (or another test for a test request)
       is brought forward here. */
    const char *requestOwner = getenv("TOTK_SLOT_OWNER");
    if (![h[@"owner"] isEqualToString:@"user"] &&
        (!requestOwner || !*requestOwner || !strcmp(requestOwner, "user"))) return 0;
    pid_t pid = [h[@"game_pid"] intValue];
    return pid > 0 && pid_start(pid) == (uint64_t)[h[@"game_start"] longLongValue] ? pid : 0;
}

static void note(NSString *text) {
    NSAlert *alert = [NSAlert new];
    alert.messageText = text;
    [alert runModal];
}

/* Every TOTK_CONTRIB_STEP shaders and render pipelines the shared list does not have yet (totk_shader_contrib.h), before the game starts: ask
   the player to send the file in a GitHub issue. "Send" opens the issue form and shows the file in Finder to drag in.
   With gh signed in, "Submit automatically" takes the place of "Remind me next time" and sends this file at once.
   The star button only opens the project page (the player stars it there) and asks again next time. */
static void contrib_reminder(NSString *path) {
    NSUInteger rows = totk_contrib_due(path);
    if (!rows) return;
    totk_contrib_reminded(path, rows);   // "next time": TOTK_CONTRIB_STEP rows later; "don't remind me again": never
    NSString *gh = gh_path();
    [NSApplication sharedApplication]; [NSApp activateIgnoringOtherApps:YES];
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:path]]];   // the folder, before the question
    NSAlert *alert = [NSAlert new];
    alert.messageText = @(TOTK_UI("期待你的贡献", "We would love your contribution"));
    alert.informativeText = [NSString stringWithFormat:@"%@%@", [NSString stringWithFormat:@(TOTK_UI("你有 %lu 条着色器和渲染管线可以贡献：玩的时候遇到的、公共着色器清单里还没有的。它们记在一个小文件里，只有着色器和管线的标识，不含游戏内容、存档或个人信息。把它发到 GitHub 加进公共清单，以后大家打包的 App 都会带上它们，玩到那里不再卡顿。\n\n",
                                      "You have %lu shaders and render pipelines to contribute: ones your game met that the shared shader list does not have yet. They are kept in a small file with shader and pipeline identifiers only: no game data, saves or personal information. Sent to GitHub and added to the shared list, they come with everyone's next build, and nobody stutters there again.\n\n")), (unsigned long)rows],
                             gh ? @(TOTK_UI("点“自动提交”会用这台 Mac 上登录的 GitHub 命令行（gh）直接提交，不用打开网页。点“发送”会打开 GitHub 的提交页面，并在访达里显示这个文件，把它拖进页面即可。",
                                            "\"Submit automatically\" sends it now with the GitHub CLI (gh) signed in on this Mac, without the browser. \"Send\" opens the GitHub form and shows the file in Finder: drag it into the form."))
                                : @(TOTK_UI("点“发送”会打开 GitHub 的提交页面，并在访达里显示这个文件，把它拖进页面即可。",
                                            "\"Send\" opens the GitHub form and shows the file in Finder: drag it into the form."))];
    [alert addButtonWithTitle:@(TOTK_UI("发送", "Send"))];
    [alert addButtonWithTitle:gh ? @(TOTK_UI("自动提交", "Submit automatically")) : @(TOTK_UI("下次提醒", "Remind me next time"))];
    [alert addButtonWithTitle:@(TOTK_UI("不要提醒了", "Don't remind me again"))];
    [alert addButtonWithTitle:@(TOTK_UI("⭐ 喜欢就点个 Star", "⭐ Like it? Star it on GitHub"))];
    NSModalResponse choice = [alert runModal];
    if (choice == NSAlertThirdButtonReturn + 1) [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@TOTK_REPO_URL]];
    if (choice == NSAlertThirdButtonReturn) totk_contrib_never(path);
    if (choice == NSAlertSecondButtonReturn && gh) {
        NSString *issue = contrib_submit(gh, path, rows);
        if (issue) { note([NSString stringWithFormat:@"%@\n%@", @(TOTK_UI("感谢你的 shader 贡献，让我们一起让游戏更流畅！", "Thank you for your shader contribution! Let's make the game smoother together!")), issue]); return; }
        note(@(TOTK_UI("自动提交没有成功，改为打开提交页面。", "Submitting automatically did not work; opening the form instead.")));
        choice = NSAlertFirstButtonReturn;
    }
    if (choice != NSAlertFirstButtonReturn) return;
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@TOTK_CONTRIB_ISSUE_URL]];
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:path]]];
}

/* The contribution file: in the project folder the app was built from, <clone>/shader-list-contribution/shader-list-contribution.txt
   (MNGContributionFolder in Info.plist, build_app.py), where players look and where the iPhone / iPad copies go too; the
   data folder when that folder is gone (user 2026-10-22: nobody found the file in ~/Library). The file that was in the data
   folder moves there once, with its reminder state. */
static NSString *contrib_file(NSString *data) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *old = [data stringByAppendingPathComponent:@"shader-list-contribution.txt"];
    NSString *folder = [NSBundle.mainBundle objectForInfoDictionaryKey:@"MNGContributionFolder"];
    if (![folder isKindOfClass:NSString.class] || !folder.length || ![fm fileExistsAtPath:folder.stringByDeletingLastPathComponent]) return old;
    if (![fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil]) return old;
    // one file for the Mac and every iPhone / iPad (ios/tools/shader_contrib_watch.py merges theirs in): the rows are the same
    // for every platform; Breath of the Wild has a list of its own
    NSString *mine = [folder stringByAppendingPathComponent:strcmp(GAME_FOLDER, "TOTK") ? @"shader-list-contribution-botw.txt" : @"shader-list-contribution.txt"];
    if (![fm fileExistsAtPath:mine] && [fm fileExistsAtPath:old]) {
        [fm moveItemAtPath:old toPath:mine error:nil];
        NSString *state = [old stringByAppendingString:@".reminded"];   // where the state was kept before (totk_contrib_state)
        if ([fm fileExistsAtPath:state]) [fm moveItemAtPath:state toPath:totk_contrib_state(mine) error:nil];
    }
    return mine;
}

static BOOL make_dir(NSString *path) {
    return [NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
}

/* build_app.py --mac-game-data library: the app carries no RomFS image. A folder holds it when its romfs.bin (in it, or in
   <folder>/<version>) has the size the build pinned (romfs-config.txt) and a romfs.sha256 beside it, if any, names the
   pinned image: the build copies to romfs.bin.partial first, so a romfs.bin of the full size is a complete copy. Its hash
   is checked by the runtime at the first start (TOTK_ROMFS_PROOF_DIR). Returns the folder of the image, or nil. */
static NSString *game_data_in(NSString *folder, unsigned long long size, NSString *sha) {
    if (!folder.length) return nil;
    for (NSString *place in @[[folder stringByAppendingPathComponent:@TOTK_GAME_DIR], folder]) {
        NSDictionary *image = [NSFileManager.defaultManager attributesOfItemAtPath:[place stringByAppendingPathComponent:@"romfs.bin"] error:nil];
        if (!image || image.fileSize != size) continue;
        NSString *mark = [NSString stringWithContentsOfFile:[place stringByAppendingPathComponent:@"romfs.sha256"] encoding:NSUTF8StringEncoding error:nil];
        mark = [mark stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!mark.length || [mark caseInsensitiveCompare:sha] == NSOrderedSame) return place;
    }
    return nil;
}

/* No folder holds the image (the app copied to another Mac, or its disk not connected): ask where it is, and keep the
   answer for the next starts (defaults MNGGameDataFolder.<version>). The image is read where it is, never moved or
   copied. Returns its folder, or nil to quit. */
static NSString *game_data_ask(NSArray<NSString *> *looked, unsigned long long size, NSString *sha) {
    [NSApplication sharedApplication]; [NSApp activateIgnoringOtherApps:YES];
    for (;;) {
        NSAlert *alert = [NSAlert new];
        alert.messageText = @(TOTK_UI("游戏数据在哪里？", "Where is the game data?"));
        alert.informativeText = [NSString stringWithFormat:@(TOTK_UI("这个 App 不包含游戏数据（romfs.bin，%.1f GB），在 %@ 里没有找到。如果它在没有连接的磁盘上，请连接后点“重试”；否则请选择存放它的文件夹。游戏会在原处读取它，不会移动或复制。",
                                                                    "This app keeps its game data (romfs.bin, %.1f GB) outside the app, and it is not in %@. If it is on a disk that is not connected, connect it and choose Try Again; otherwise choose the folder it is in. The game reads it where it is, without moving or copying it.")),
                                     size / 1e9, [looked componentsJoinedByString:@(TOTK_UI("、", ", "))]];
        [alert addButtonWithTitle:@(TOTK_UI("选择文件夹…", "Choose Folder…"))];
        [alert addButtonWithTitle:@(TOTK_UI("重试", "Try Again"))];
        [alert addButtonWithTitle:@(TOTK_UI("退出", "Quit"))];
        NSModalResponse choice = [alert runModal];
        if (choice == NSAlertThirdButtonReturn) return nil;
        if (choice == NSAlertSecondButtonReturn) {
            for (NSString *folder in looked) { NSString *found = game_data_in(folder, size, sha); if (found) return found; }
            continue;
        }
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO; panel.canChooseDirectories = YES; panel.allowsMultipleSelection = NO;
        panel.message = @(TOTK_UI("选择存放 romfs.bin 的文件夹（或它上一层、以版本命名的文件夹所在的文件夹）。",
                                  "Choose the folder that holds romfs.bin (or the folder that holds its " TOTK_GAME_DIR " folder)."));
        panel.prompt = @(TOTK_UI("选择", "Choose"));
        if ([panel runModal] != NSModalResponseOK || !panel.URL) continue;
        NSString *found = game_data_in(panel.URL.path, size, sha);
        if (found) {
            [NSUserDefaults.standardUserDefaults setObject:found forKey:@"MNGGameDataFolder." TOTK_GAME_DIR];
            return found;
        }
        note([NSString stringWithFormat:@(TOTK_UI("%@ 里没有这个版本的 romfs.bin（应为 %llu 字节）。", "%@ holds no romfs.bin of this version (%llu bytes).")),
              panel.URL.path, size]);
    }
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"totk"];
        NSString *binary = [root stringByAppendingPathComponent:@"build/totk-native"];
        /* --check (build_app.py, once after packaging): only look that the game is there and exit. The first run of a new
           app is when macOS scans the whole bundle (minutes for 16 GB, with nothing on screen); doing it during the build
           makes the player's first start immediate. */
        if (argc > 1 && !strcmp(argv[1], "--check")) {
            int ok = access(binary.fileSystemRepresentation, X_OK) == 0;
            NSString *declared = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
            ok = ok && [declared isEqualToString:@TOTK_GAME_VERSION];
            printf("%s: game version %s\n", ok ? "ok" : "missing binary or version mismatch", TOTK_GAME_VERSION);
            return ok ? 0 : 1;
        }
        if (access(binary.fileSystemRepresentation, X_OK) != 0) {
            show_error(@"The game files are missing from this app. Build it again with build_app.py.");
            return 1;
        }
        NSString *home = NSHomeDirectory();
        NSString *data = [home stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/" GAME_FOLDER];   /* game_version.h: TOTK, BotW */
        NSString *logs = [home stringByAppendingPathComponent:@"Library/Logs/MacNativeGames"];
        /* Folders the environment already names are kept (open --env TOTK_PROFILE_DIR=... runs a test on a save copy,
           TOTK_GAME_SLOT_DIR shares one game slot with another build of the game: one game at a time on the Mac). */
        NSString *(^env_or)(const char *, NSString *) = ^NSString *(const char *name, NSString *fallback) {
            const char *v = getenv(name); return v && *v ? @(v) : fallback; };
        NSString *profile = env_or("TOTK_PROFILE_DIR", [data stringByAppendingPathComponent:@"profile"]);
        NSString *slot = env_or("TOTK_GAME_SLOT_DIR", [data stringByAppendingPathComponent:@"game-slot"]);
        NSString *cache = env_or("TOTK_SHADER_CACHE", [data stringByAppendingPathComponent:[NSString stringWithFormat:@"shader-cache/rt-%s", DMSH_CHAIN]]);
        if (!make_dir(profile) || !make_dir(slot) || !make_dir(cache) || !make_dir(logs)) {
            show_error([NSString stringWithFormat:@"Cannot create %@.", data]);
            return 1;
        }
        pid_t running = running_game(slot);
        if (running) {
            [[NSRunningApplication runningApplicationWithProcessIdentifier:running] activateWithOptions:NSApplicationActivateAllWindows];
            return 0;
        }
        mng_update_notice(data);
#if defined(TOTK_GAME_V112) || defined(TOTK_GAME_V121) || defined(TOTK_GAME_V143)
        NSString *backup = [profile stringByAppendingFormat:@"-before-%s", TOTK_GAME_VERSION];
        NSFileManager *fm = NSFileManager.defaultManager;
        if (![fm fileExistsAtPath:backup]) {
            NSString *temporary = [backup stringByAppendingFormat:@".copy-%d", getpid()];
            NSError *error = nil;
            if (![fm copyItemAtPath:profile toPath:temporary error:&error] ||
                ![fm moveItemAtPath:temporary toPath:backup error:&error]) {
                [fm removeItemAtPath:temporary error:nil];
                show_error([@"Cannot back up saves before upgrading: " stringByAppendingString:error.localizedDescription ?: @"unknown error"]);
                return 1;
            }
        }
#endif
        if (chdir(root.fileSystemRepresentation) != 0) {   // the game reads its data relative to this folder
            show_error(@"Cannot open the game folder inside the app.");
            return 1;
        }
        /* The player's controller layout (runtime/controller_map.h), checked here: a bad line shows this alert instead of
           the game closing at start. */
        NSString *controls = env_or("TOTK_CONTROLS", [data stringByAppendingPathComponent:@"controls.txt"]);
        NSData *layout = [NSData dataWithContentsOfFile:controls];
        if (layout) {
            NSString *text = layout.length < 16384 ? [[NSString alloc] initWithData:layout encoding:NSUTF8StringEncoding] : nil;
            uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS]; controller_map_default(map, modes);
            const char *problem = layout.length < 16384 ? "not UTF-8 text" : "larger than 16 KB";
            int line = text ? controller_map_parse(text.UTF8String, map, modes, &problem) : 1;
            if (line) { show_error([NSString stringWithFormat:@"%@, line %d: %s", controls, line, problem]); return 1; }
        }
        setenv("TOTK_CONTROLS", controls.fileSystemRepresentation, 1);
        /* key 3's picture mode, kept between runs (runtime/game_patches_v143.c picture_mode_save / _restore) */
        setenv("TOTK_PICTURE_MODE", env_or("TOTK_PICTURE_MODE", [data stringByAppendingPathComponent:@"picture-mode.txt"]).fileSystemRepresentation, 1);
        setenv("TOTK_PROFILE_DIR", profile.fileSystemRepresentation, 1);
        setenv("TOTK_SHADER_CACHE", cache.fileSystemRepresentation, 1);
        setenv("TOTK_GAME_SLOT_DIR", slot.fileSystemRepresentation, 1);
        setenv("TOTK_ROMFS_PROOF_DIR", data.fileSystemRepresentation, 1);   // the app bundle stays read-only
        /* build_app.py --mac-game-data library: the app carries no RomFS image, so a new build is a few hundred MB and
           copies to another Mac without it. It is looked for in the folder chosen at an earlier start (game_data_ask), the
           folder the build put it in (Info.plist MNGGameDataFolder, e.g. on another disk), and <data>/GameData; with none
           holding it, the player is asked. An app with its own image uses that. */
        NSFileManager *files = NSFileManager.defaultManager;
        if (![files fileExistsAtPath:@"extracted/" TOTK_GAME_DIR "/romfs/romfs.bin"]) {
            NSString *pins = [NSString stringWithContentsOfFile:@"build/game-data/" TOTK_GAME_DIR "/romfs-config.txt" encoding:NSUTF8StringEncoding error:nil];
            unsigned long long size = 0; NSString *sha = @"";
            for (NSString *line in [pins componentsSeparatedByString:@"\n"]) {
                NSArray<NSString *> *kv = [line componentsSeparatedByString:@" "];
                if (kv.count != 2) continue;
                if ([kv[0] isEqualToString:@"ROMFS_IMAGE_SIZE"]) size = strtoull(kv[1].UTF8String, NULL, 10);
                if ([kv[0] isEqualToString:@"ROMFS_IMAGE_SHA256"]) sha = kv[1];
            }
            if (!size) { show_error(@"The game files are missing from this app. Build it again with build_app.py."); return 1; }
            NSMutableArray<NSString *> *places = [NSMutableArray array];
            for (id place in @[[NSUserDefaults.standardUserDefaults stringForKey:@"MNGGameDataFolder." TOTK_GAME_DIR] ?: @"",
                               [NSBundle.mainBundle objectForInfoDictionaryKey:@"MNGGameDataFolder"] ?: @"",
                               [data stringByAppendingPathComponent:@"GameData"]])
                if ([place isKindOfClass:NSString.class] && [place length] && ![places containsObject:place]) [places addObject:place];
            NSString *library = nil;
            for (NSString *place in places) if ((library = game_data_in(place, size, sha))) break;
            if (!library && !(library = game_data_ask(places, size, sha))) return 0;
            setenv("TOTK_ROMFS_DATA", library.fileSystemRepresentation, 1);   // runtime/romfs.c
        }
        setenv("TOTK_SHARED_FONTS", "extracted/firmware/shared-fonts", 1);
        /* Player mods (runtime/romfs.c): <data>/mods/<version>/manifest.tsv, when present. With no controller connected at
           start, <data>/mods/<version>-keyboard takes its place when it holds a manifest (keyboard key icons in the prompts:
           scripts/make_button_icons.py --style keyboard or azerty). The fonts are read once, so it holds for the session. */
        NSString *mods = [data stringByAppendingPathComponent:@"mods/" TOTK_GAME_DIR];
        NSString *keyboardMods = [mods stringByAppendingString:@"-keyboard"];
        if (!getenv("TOTK_ROMFS_MODS") && [NSFileManager.defaultManager fileExistsAtPath:[keyboardMods stringByAppendingPathComponent:@"manifest.tsv"]] &&
            !controller_connected())
            mods = keyboardMods;
        setenv("TOTK_ROMFS_MODS", mods.fileSystemRepresentation, 0);
        /* The game's language (runtime/services.c): the app's first preferred language, which also follows a per-app
           language (defaults write org.macnativegames.totk AppleLanguages -array pt-BR). TOTK_LANG still overrides it. */
        NSString *preferred = NSLocale.preferredLanguages.firstObject;
        if (preferred) setenv("TOTK_LOCALE", preferred.UTF8String, 1);
        setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
        setenv("TOTK_GPU_WIDEN", "1", 1);
        setenv("TOTK_GPU_BLOCKRT", "1", 1);
        setenv("TOTK_GPU_MIDCLEAR", "1", 1);
        setenv("TOTK_SLOT_OWNER", "user", 0);
        setenv("TOTK_SLOT_PURPOSE", "player", 0);
        /* The commit the app was built from (build_app.py MNGSourceCommit): the HUD box shows it (runtime/frontend.m). */
        NSString *commit = [NSBundle.mainBundle objectForInfoDictionaryKey:@"MNGSourceCommit"];
        if ([commit isKindOfClass:NSString.class] && commit.length) setenv("TOTK_SOURCE_COMMIT", commit.UTF8String, 1);
        NSString *contrib = env_or("TOTK_SHADER_CONTRIB", contrib_file(data));
        setenv("TOTK_SHADER_CONTRIB", contrib.fileSystemRepresentation, 1);
        contrib_reminder(contrib);
        unsetenv("TOTK_AUTOLOAD");   // the game starts at its title screen, as on the console
        /* runtime command channel (runtime/input.c): echo "shot ~/Desktop/shot.png" >> "<data>/input.txt" */
        { NSString *cmds = env_or("TOTK_INPUT_FILE", [data stringByAppendingPathComponent:@"input.txt"]);
          FILE *cf = fopen(cmds.fileSystemRepresentation, "w"); if (cf) fclose(cf);
          setenv("TOTK_INPUT_FILE", cmds.fileSystemRepresentation, 1); }

        /* The log is written only with debug on, as on the iPhone: defaults write org.macnativegames.totk TOTK_DEBUG_LOG
           -bool YES (org.macnativegames.botw for Breath of the Wild), or TOTK_DEBUG_LOG=1 in the environment, which wins
           (the test tools pass it). Off, the game's output goes to /dev/null and the logs of earlier runs are removed. */
        const char *debugEnv = getenv("TOTK_DEBUG_LOG");
        BOOL debugLog = debugEnv ? (*debugEnv && strcmp(debugEnv, "0")) : [NSUserDefaults.standardUserDefaults boolForKey:@"TOTK_DEBUG_LOG"];
        NSString *log = [logs stringByAppendingPathComponent:@GAME_LOG ".log"];
        NSString *previous = [logs stringByAppendingPathComponent:@GAME_LOG ".previous.log"];
        [NSFileManager.defaultManager removeItemAtPath:previous error:nil];
        if (debugLog) [NSFileManager.defaultManager moveItemAtPath:log toPath:previous error:nil];
        else [NSFileManager.defaultManager removeItemAtPath:log error:nil];
        FILE *lf = fopen(debugLog ? log.fileSystemRepresentation : "/dev/null", "w");
        if (lf) {
            dup2(fileno(lf), STDOUT_FILENO);
            dup2(fileno(lf), STDERR_FILENO);
            fclose(lf);
        }
        char *const arguments[] = {(char *)binary.fileSystemRepresentation, "rewritten/" TOTK_GAME_DIR, "boot", NULL};
        execv(arguments[0], arguments);
        int failed = errno;
        fprintf(stderr, "[launcher] execv failed: %s\n", strerror(failed));
        show_error([NSString stringWithFormat:@"Starting the game failed: %s.", strerror(failed)]);
        return 1;
    }
}
