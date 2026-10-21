// The launcher inside the packaged app (build_app.py compiles it as Contents/MacOS/TOTK).
// The game itself is Contents/Resources/totk/build/totk-native; everything it writes goes to the player's own folders:
//   ~/Library/Application Support/MacNativeGames/TOTK/   profile (account, saves), shader cache, game slot, RomFS proof (BotW/ for Breath of the Wild)
//   ~/Library/Logs/MacNativeGames/totk.log             this run (totk.previous.log: the one before; botw.log for Breath of the Wild)
// If the game is already running, the launcher only brings it to the front.
#import <AppKit/AppKit.h>
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
#include "launch_settings.h"       // the settings window's settings, checks and precedence
#import "settings_window.h"

static void show_error(NSString *detail) {
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"The game could not start";
    alert.informativeText = detail;
    [alert runModal];
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

/* A tool's standard output and exit status (input, if any, goes to its standard input); nil if it did not start. */
static NSString *run_tool(NSString *exe, NSArray<NSString *> *args, NSString *input, int *status) {
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

/* The GitHub CLI when it is installed and signed in, else nil (an app started from Finder has no Homebrew in its PATH). */
static NSString *gh_path(void) {
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
static NSString *contrib_submit(NSString *gh, NSString *path, NSUInteger rows) {
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

static void note(NSString *text) {
    NSAlert *alert = [NSAlert new];
    alert.messageText = text;
    [alert runModal];
}

/* Every TOTK_CONTRIB_STEP shaders the shared list does not have yet (totk_shader_contrib.h), before the game starts: ask
   the player to send the file in a GitHub issue. "Send" opens the issue form and shows the file in Finder to drag in.
   With gh signed in, "Submit automatically" takes the place of "Remind me next time" and sends this file at once. */
static void contrib_reminder(NSString *path) {
    NSUInteger rows = totk_contrib_due(path);
    if (!rows) return;
    totk_contrib_reminded(path, rows);   // "next time": TOTK_CONTRIB_STEP rows later; "don't remind me again": never
    NSString *gh = gh_path();
    [NSApplication sharedApplication]; [NSApp activateIgnoringOtherApps:YES];
    NSAlert *alert = [NSAlert new];
    alert.messageText = @(TOTK_UI("期待你的贡献", "We would love your contribution"));
    alert.informativeText = [NSString stringWithFormat:@"%@%@", [NSString stringWithFormat:@(TOTK_UI("你有 %lu 个着色器可以贡献：玩的时候遇到的、公共着色器清单里还没有的。它们记在一个小文件里，只有着色器的标识，不含游戏内容、存档或个人信息。把它发到 GitHub 加进公共清单，以后大家打包的 App 都会带上它们，玩到那里不再卡顿。\n\n",
                                      "You have %lu shaders to contribute: ones your game met that the shared shader list does not have yet. They are kept in a small file with shader identifiers only: no game data, saves or personal information. Sent to GitHub and added to the shared list, they come with everyone's next build, and nobody stutters there again.\n\n")), (unsigned long)rows],
                             gh ? @(TOTK_UI("点“自动提交”会用这台 Mac 上登录的 GitHub 命令行（gh）直接提交，不用打开网页。点“发送”会打开 GitHub 的提交页面，并在访达里显示这个文件，把它拖进页面即可。",
                                            "\"Submit automatically\" sends it now with the GitHub CLI (gh) signed in on this Mac, without the browser. \"Send\" opens the GitHub form and shows the file in Finder: drag it into the form."))
                                : @(TOTK_UI("点“发送”会打开 GitHub 的提交页面，并在访达里显示这个文件，把它拖进页面即可。",
                                            "\"Send\" opens the GitHub form and shows the file in Finder: drag it into the form."))];
    [alert addButtonWithTitle:@(TOTK_UI("发送", "Send"))];
    [alert addButtonWithTitle:gh ? @(TOTK_UI("自动提交", "Submit automatically")) : @(TOTK_UI("下次提醒", "Remind me next time"))];
    [alert addButtonWithTitle:@(TOTK_UI("不要提醒了", "Don't remind me again"))];
    NSModalResponse choice = [alert runModal];
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

static BOOL make_dir(NSString *path) {
    return [NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
}

/* The saved settings (app/launch_settings.h): this app's defaults, key LaunchEnv (name -> value); only string values. */
static NSString *const kLaunchEnv = @"LaunchEnv", *const kShowSettings = @"ShowLaunchSettings";
static NSMutableDictionary<NSString *, NSString *> *saved_settings(void) {
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    NSDictionary *d = [NSUserDefaults.standardUserDefaults dictionaryForKey:kLaunchEnv];
    for (NSString *k in d) if ([k isKindOfClass:NSString.class] && [d[k] isKindOfClass:NSString.class]) out[k] = d[k];
    return out;
}
static void save_settings(NSDictionary<NSString *, NSString *> *values, BOOL showAlways) {
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    if (values.count) [d setObject:values forKey:kLaunchEnv]; else [d removeObjectForKey:kLaunchEnv];
    if (showAlways) [d setBool:YES forKey:kShowSettings]; else [d removeObjectForKey:kShowSettings];
    CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);   // written before the launcher execs the game or exits
}
/* One settings window at a time: while it is up, its launcher holds an exclusive lock on <data>/settings-window.lock, with
   its pid inside (the lock goes with the process, or at the exec into the game). Another launcher that wants the window
   (Command-comma again, Option again) brings that one to the front and returns NO. */
static BOOL settings_window_free(NSString *data) {
    static int fd = -1;
    if (fd >= 0) return YES;
    make_dir(data);
    fd = open([data stringByAppendingPathComponent:@"settings-window.lock"].fileSystemRepresentation, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return YES;   // no lock file: show the window anyway
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        char pid[16]; int n = snprintf(pid, sizeof pid, "%d\n", getpid());
        if (ftruncate(fd, 0) == 0) (void)!pwrite(fd, pid, (size_t)n, 0);
        return YES;
    }
    char text[16] = {0}; (void)!pread(fd, text, sizeof text - 1, 0);
    close(fd); fd = -1;
    pid_t other = atoi(text);
    NSRunningApplication *app = other > 0 ? [NSRunningApplication runningApplicationWithProcessIdentifier:other] : nil;
    [app activateWithOptions:NSApplicationActivateAllWindows];
    return NO;
}
typedef struct { __unsafe_unretained NSDictionary<NSString *, NSString *> *values; __unsafe_unretained NSSet<NSString *> *env; } ResolveCtx;
static const char *resolve_saved(const char *name, void *ctx) { NSString *v = ((ResolveCtx *)ctx)->values[@(name)]; return v.UTF8String; }
static int resolve_env(const char *name, void *ctx) { return [((ResolveCtx *)ctx)->env containsObject:@(name)]; }

/* Why the settings window opens by itself, or nil: the last start failed on one of its settings (the previous log's
   FAIL / FATAL line), or the last game crashed within a minute of starting (the game slot's history). */
static NSString *last_start_problem(NSString *log, NSString *slot, NSString **setting) {
    NSFileHandle *f = [NSFileHandle fileHandleForReadingAtPath:log];   // the last megabyte: a long game writes a long log
    unsigned long long size = [f seekToEndOfFile];
    [f seekToFileOffset:size > (1 << 20) ? size - (1 << 20) : 0];
    NSMutableData *tail = [[f readDataToEndOfFile] mutableCopy]; [f closeFile];
    [tail appendBytes:"" length:1];   // as a C string; the lines searched for are ASCII
    char line[512] = "";
    const LaunchSetting *s = f ? launch_failed_setting(tail.bytes, line, sizeof line) : NULL;
    if (s) {
        *setting = @(s->name);
        return [NSString stringWithFormat:@(TOTK_UI("上次启动因为 %s 的值停止了：%s", "The last start stopped because of %s: %s")), s->name, line];
    }
    NSString *history = [NSString stringWithContentsOfFile:[slot stringByAppendingPathComponent:@"history.log"] encoding:NSUTF8StringEncoding error:nil];
    NSDate *died = [NSFileManager.defaultManager attributesOfItemAtPath:log error:nil].fileModificationDate;
    long ran = history && died ? launch_early_crash(history.UTF8String, (time_t)died.timeIntervalSince1970, 60) : -1;
    if (ran < 0) return nil;
    return [NSString stringWithFormat:@(TOTK_UI("上次游戏启动 %ld 秒后意外退出（%s）。可以在这里改回设置再试。", "The last game ended unexpectedly %ld s after it started (%s). You can change settings back here and try again.")),
                                      ran, log.lastPathComponent.UTF8String];
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
        /* Option held while the app opens (or --settings / TOTK_SETTINGS=1): the settings window. Read first, while the key
           is still down; the settings the environment already gives are noted before the launcher sets anything. */
        BOOL wantSettings = (NSEvent.modifierFlags & NSEventModifierFlagOption) != 0, editOnly = NO;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--settings")) wantSettings = YES;
            if (!strcmp(argv[i], "--settings-only")) editOnly = YES;   // the game's Settings… (runtime/frontend.m)
        }
        { const char *e = getenv("TOTK_SETTINGS"); if (e && *e && strcmp(e, "0")) wantSettings = YES; }
        NSMutableSet<NSString *> *fromEnv = [NSMutableSet set];
        for (int i = 0; i < LAUNCH_SETTINGS; i++) if (getenv(launch_settings[i].name)) [fromEnv addObject:@(launch_settings[i].name)];
        /* --settings-only: the game's Settings… while it runs. Only the window and the saved settings: no game, no slot, no
           log; the player quits and reopens the game for them to apply. */
        if (editOnly) {
            if (!settings_window_free([NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/" GAME_FOLDER])) return 0;
            NSMutableDictionary<NSString *, NSString *> *values = saved_settings();
            NSString *savedUi = values[@"TOTK_UI_LANG"];
            if (![fromEnv containsObject:@"TOTK_UI_LANG"] && savedUi && !launch_setting_check("TOTK_UI_LANG", savedUi.UTF8String))
                setenv("TOTK_UI_LANG", savedUi.UTF8String, 1);
            BOOL showAlways = [NSUserDefaults.standardUserDefaults boolForKey:kShowSettings];
            if (launch_settings_window(values, fromEnv, nil, nil, &showAlways, YES) != LaunchSave) return 0;
            save_settings(values, showAlways);
            note(@(TOTK_UI("设置已保存。退出游戏（⌘Q）再重新打开后生效。", "Settings saved. Quit the game (⌘Q) and open it again for them to apply.")));
            return 0;
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
        /* The game's settings (app/launch_settings.h): environment > saved > the game's default. The window opens when asked
           for, when the player chose "show at every start", or when the last start failed (last_start_problem). */
        NSString *log = [logs stringByAppendingPathComponent:@GAME_LOG ".log"];
        NSMutableDictionary<NSString *, NSString *> *values = saved_settings();
        NSString *savedUi = values[@"TOTK_UI_LANG"];   // the window's own language follows the saved launcher language
        if (![fromEnv containsObject:@"TOTK_UI_LANG"] && savedUi && !launch_setting_check("TOTK_UI_LANG", savedUi.UTF8String))
            setenv("TOTK_UI_LANG", savedUi.UTF8String, 1);
        NSString *problemSetting = nil, *problem = last_start_problem(log, slot, &problemSetting);
        BOOL showAlways = [NSUserDefaults.standardUserDefaults boolForKey:kShowSettings], once = NO;
        if (wantSettings || showAlways || problem) {
            if (!settings_window_free(data)) return 0;   // already open: brought to the front
            LaunchChoice choice = launch_settings_window(values, fromEnv, problem, problemSetting, &showAlways, NO);
            if (choice == LaunchQuit) return 0;
            once = choice == LaunchOnce;
            if (choice == LaunchSave) save_settings(values, showAlways);
        }
        LaunchValue resolved[LAUNCH_SETTINGS];
        ResolveCtx ctx = {values, fromEnv};
        launch_settings_resolve(resolve_saved, resolve_env, &ctx, resolved);
        NSMutableArray<NSString *> *settingsLog = [NSMutableArray array];   // written to the game's log below
        for (int i = 0; i < LAUNCH_SETTINGS; i++) {
            const LaunchValue *r = &resolved[i];
            if (r->source == LAUNCH_SAVED) {
                setenv(r->name, r->value, 1);
                [settingsLog addObject:[NSString stringWithFormat:@"%s=%s (%s)", r->name, r->value, once ? "this launch only" : "saved"]];
            } else if (r->source == LAUNCH_ENV) {
                [settingsLog addObject:[NSString stringWithFormat:@"%s=%s (environment%@)", r->name, getenv(r->name),
                                                                  r->value ? [NSString stringWithFormat:@"; the saved %s is not used", r->value] : @""]];
            } else {
                unsetenv(r->name);
                if (r->source == LAUNCH_IGNORED)
                    [settingsLog addObject:[NSString stringWithFormat:@"ignored saved %s=\"%s\": %s", r->name, r->value, r->problem]];
            }
        }
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
        setenv("TOTK_PROFILE_DIR", profile.fileSystemRepresentation, 1);
        setenv("TOTK_SHADER_CACHE", cache.fileSystemRepresentation, 1);
        setenv("TOTK_GAME_SLOT_DIR", slot.fileSystemRepresentation, 1);
        setenv("TOTK_ROMFS_PROOF_DIR", data.fileSystemRepresentation, 1);   // the app bundle stays read-only
        setenv("TOTK_APP_BUNDLE", NSBundle.mainBundle.bundlePath.fileSystemRepresentation, 1);   // the game's Settings… opens it again
        setenv("TOTK_SHARED_FONTS", "extracted/firmware/shared-fonts", 1);
        /* Player mods (runtime/romfs.c): <data>/mods/<version>/manifest.tsv, when present. */
        setenv("TOTK_ROMFS_MODS", [data stringByAppendingPathComponent:@"mods/" TOTK_GAME_DIR].fileSystemRepresentation, 0);
        /* The game's language (runtime/services.c): the app's first preferred language, which also follows a per-app
           language (defaults write org.macnativegames.totk AppleLanguages -array pt-BR). TOTK_LANG still overrides it. */
        NSString *preferred = NSLocale.preferredLanguages.firstObject;
        if (preferred) setenv("TOTK_LOCALE", preferred.UTF8String, 1);
        setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
        setenv("TOTK_GPU_WIDEN", "1", 1);
        setenv("TOTK_GPU_BLOCKRT", "1", 1);
        setenv("TOTK_GPU_MIDCLEAR", "1", 1);
        setenv("TOTK_GPU_BUDGET_MB", "5000", 1);
        setenv("TOTK_SLOT_OWNER", "user", 0);
        setenv("TOTK_SLOT_PURPOSE", "player", 0);
        NSString *contrib = env_or("TOTK_SHADER_CONTRIB", [data stringByAppendingPathComponent:@"shader-list-contribution.txt"]);
        setenv("TOTK_SHADER_CONTRIB", contrib.fileSystemRepresentation, 1);
        contrib_reminder(contrib);
        unsetenv("TOTK_AUTOLOAD");   // the game starts at its title screen, as on the console
        /* runtime command channel (runtime/input.c): echo "shot ~/Desktop/shot.png" >> "<data>/input.txt" */
        { NSString *cmds = env_or("TOTK_INPUT_FILE", [data stringByAppendingPathComponent:@"input.txt"]);
          FILE *cf = fopen(cmds.fileSystemRepresentation, "w"); if (cf) fclose(cf);
          setenv("TOTK_INPUT_FILE", cmds.fileSystemRepresentation, 1); }

        [NSFileManager.defaultManager removeItemAtPath:[logs stringByAppendingPathComponent:@GAME_LOG ".previous.log"] error:nil];
        [NSFileManager.defaultManager moveItemAtPath:log toPath:[logs stringByAppendingPathComponent:@GAME_LOG ".previous.log"] error:nil];
        FILE *lf = fopen(log.fileSystemRepresentation, "w");
        if (lf) {
            dup2(fileno(lf), STDOUT_FILENO);
            dup2(fileno(lf), STDERR_FILENO);
            fclose(lf);
        }
        fprintf(stderr, "[launcher] settings (defaults %s %s; hold Option while opening the game to change them):%s\n",
                NSBundle.mainBundle.bundleIdentifier.UTF8String ?: "?", kLaunchEnv.UTF8String, settingsLog.count ? "" : " the game's defaults");
        for (NSString *line in settingsLog) fprintf(stderr, "[launcher]   %s\n", line.UTF8String);
        char *const arguments[] = {(char *)binary.fileSystemRepresentation, "rewritten/" TOTK_GAME_DIR, "boot", NULL};
        execv(arguments[0], arguments);
        fprintf(stderr, "[launcher] execv failed: %s\n", strerror(errno));
        show_error([NSString stringWithFormat:@"Starting the game failed. See %@.", log]);
        return 1;
    }
}
