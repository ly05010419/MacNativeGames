// The launcher inside Super Smash Bros. Ultimate.app: this folder's copy of games/totk/app/launcher.m, by way of the
// Super Mario Odyssey copy it was forked from. scripts/build_ssbu.py compiles it as Contents/MacOS/SSBU with this folder's
// runtime/game_version.h. The game itself is Contents/Resources/totk/build/totk-native; everything it writes goes to the
// player's own folders:
//   ~/Library/Application Support/MacNativeGames/SSBU/   profile (account, saves), shader cache, game slot, render-size.txt
//   ~/Library/Logs/MacNativeGames/ssbu.log             this run (ssbu.previous.log: the one before)
// One change from Tears of the Kingdom's: no reminder to send the shader list contribution, because the shared list it
// would go to is Tears of the Kingdom's; the rows are still written to <data>/shader-list-contribution.txt. Instead the
// notes reminder (smo_notes_reminder, named after the copy it came from) says when the app lacks enough of them that a
// rebuild is worth it. If the game is already running, the launcher only brings it to the front.
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

/* Every TOTK_CONTRIB_STEP shaders and render pipelines the shared list does not have yet (totk_shader_contrib.h), before the game starts: ask
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
    alert.informativeText = [NSString stringWithFormat:@"%@%@", [NSString stringWithFormat:@(TOTK_UI("你有 %lu 条着色器和渲染管线可以贡献：玩的时候遇到的、公共着色器清单里还没有的。它们记在一个小文件里，只有着色器和管线的标识，不含游戏内容、存档或个人信息。把它发到 GitHub 加进公共清单，以后大家打包的 App 都会带上它们，玩到那里不再卡顿。\n\n",
                                      "You have %lu shaders and render pipelines to contribute: ones your game met that the shared shader list does not have yet. They are kept in a small file with shader and pipeline identifiers only: no game data, saves or personal information. Sent to GitHub and added to the shared list, they come with everyone's next build, and nobody stutters there again.\n\n")), (unsigned long)rows],
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

/* The game's notes go into the next app build (scripts/build_ssbu.py reads them), not to the shared list. Every
   TOTK_CONTRIB_STEP rows more than this app has built in (build/notes-rows.txt, written by that build) and than at the
   last reminder, the launcher says so before the game starts. Nothing is sent anywhere. */
static void smo_notes_reminder(NSString *path, NSString *root) {
    NSString *last = [NSString stringWithContentsOfFile:[path stringByAppendingString:@".reminded"] encoding:NSUTF8StringEncoding error:nil];
    if ([last hasPrefix:@"never"]) return;
    NSUInteger rows = totk_contrib_read(path).count;
    NSString *b = [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"build/notes-rows.txt"] encoding:NSUTF8StringEncoding error:nil];
    NSUInteger built = b ? (NSUInteger)strtoull(b.UTF8String, NULL, 10) : 0;
    NSUInteger at = last ? (NSUInteger)strtoull(last.UTF8String, NULL, 10) : 0;
    NSUInteger base = built > at ? built : at;
    if (rows < base + TOTK_CONTRIB_STEP) return;
    totk_contrib_reminded(path, rows);   // "later": TOTK_CONTRIB_STEP rows later; "don't remind me again": never
    [NSApplication sharedApplication]; [NSApp activateIgnoringOtherApps:YES];
    NSAlert *alert = [NSAlert new];
    alert.messageText = @(TOTK_UI(GAME_NAME " 有新的着色器记录", "New shader notes for " GAME_NAME));
    alert.informativeText = [NSString stringWithFormat:@(TOTK_UI(
        GAME_NAME " 记录了 %lu 个这个 App 还没内置的着色器和渲染管线。重新构建 App（scripts/build_ssbu.py）会把它们内置进去，下次这些地方启动时不再卡顿。记录只留在这台 Mac 上，不会发送。",
        GAME_NAME " noted %lu shaders and render pipelines this app does not have built in yet. Rebuilding the app (scripts/build_ssbu.py) "
        "builds them in, so those places start without a stutter next time. The notes stay on this Mac: nothing is sent.")),
        (unsigned long)(rows > built ? rows - built : rows)];
    [alert addButtonWithTitle:@(TOTK_UI("显示文件", "Show the file"))];
    [alert addButtonWithTitle:@(TOTK_UI("稍后提醒", "Remind me later"))];
    [alert addButtonWithTitle:@(TOTK_UI("不要提醒了", "Don't remind me again"))];
    NSModalResponse choice = [alert runModal];
    if (choice == NSAlertThirdButtonReturn) totk_contrib_never(path);
    if (choice == NSAlertFirstButtonReturn) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:path]]];
}

static BOOL make_dir(NSString *path) {
    return [NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
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
        { /* 5000 MB was sized for 16 GB Macs; scale with memory (30%, never below 5000) so native 4K fits on bigger
             Macs. An explicit TOTK_GPU_BUDGET_MB from the environment still wins. */
          unsigned long long mb = NSProcessInfo.processInfo.physicalMemory / 10 * 3 >> 20;
          char budget[32]; snprintf(budget, sizeof budget, "%llu", mb < 5000 ? 5000ULL : mb);
          setenv("TOTK_GPU_BUDGET_MB", budget, 0); }
        setenv("TOTK_SLOT_OWNER", "user", 0);
        setenv("TOTK_SLOT_PURPOSE", "player", 0);
        /* Q80: the 3D picture size from the game's Picture menu (runtime/frontend.m), which writes <data>/render-size.txt:
           Smash makes its render images once at start, so the size is passed here as TOTK_RENDER_RES (an explicit one
           from the environment still wins). The game restarts itself through this app (SSBU_APP_BUNDLE, runtime/relaunch.c),
           so a language change starts it the way the player does. */
        NSString *sizeFile = [data stringByAppendingPathComponent:@"render-size.txt"];
        NSString *size = [[NSString stringWithContentsOfFile:sizeFile encoding:NSUTF8StringEncoding error:nil]
                          stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        const char *sizeNote = NULL;
        if ([size isEqualToString:@"2k"]) setenv("TOTK_RENDER_RES", "2560x1440", 0);
        else if (size.length && ![size isEqualToString:@"1080p"]) sizeNote = "render-size.txt is neither 1080p nor 2k: 1080p";
        setenv("SSBU_RENDER_SIZE_FILE", sizeFile.fileSystemRepresentation, 1);
        setenv("SSBU_APP_BUNDLE", NSBundle.mainBundle.bundlePath.fileSystemRepresentation, 1);
        /* Q43 (2026-10-23): build_app.py builds Smash's app without prepared startup shaders (the shader package is Q44), and
           without them every shader is translated in play and looks new to the list (totk_shader_contrib.h): the first app
           start to the title wrote 48 rows, and at TOTK_CONTRIB_STEP rows the reminder below would ask for a rebuild that
           takes no notes for this game yet. Both wait for the seed in the app. */
        if ([NSFileManager.defaultManager fileExistsAtPath:[root stringByAppendingPathComponent:@"build/shader-seed/seed-manifest.json"]]) {
            NSString *contrib = env_or("TOTK_SHADER_CONTRIB", [data stringByAppendingPathComponent:@"shader-list-contribution.txt"]);
            setenv("TOTK_SHADER_CONTRIB", contrib.fileSystemRepresentation, 1);
            smo_notes_reminder(contrib, root);
        } else unsetenv("TOTK_SHADER_CONTRIB");
        (void)contrib_reminder;   // not the shared list's reminder (see the top of this file)
        unsetenv("TOTK_AUTOLOAD");   // the game starts at its title screen, as on the console
        /* runtime command channel (runtime/input.c): echo "shot ~/Desktop/shot.png" >> "<data>/input.txt" */
        { NSString *cmds = env_or("TOTK_INPUT_FILE", [data stringByAppendingPathComponent:@"input.txt"]);
          FILE *cf = fopen(cmds.fileSystemRepresentation, "w"); if (cf) fclose(cf);
          setenv("TOTK_INPUT_FILE", cmds.fileSystemRepresentation, 1); }

        NSString *log = [logs stringByAppendingPathComponent:@GAME_LOG ".log"];
        [NSFileManager.defaultManager removeItemAtPath:[logs stringByAppendingPathComponent:@GAME_LOG ".previous.log"] error:nil];
        [NSFileManager.defaultManager moveItemAtPath:log toPath:[logs stringByAppendingPathComponent:@GAME_LOG ".previous.log"] error:nil];
        FILE *lf = fopen(log.fileSystemRepresentation, "w");
        if (lf) {
            dup2(fileno(lf), STDOUT_FILENO);
            dup2(fileno(lf), STDERR_FILENO);
            fclose(lf);
        }
        if (sizeNote) fprintf(stderr, "[launcher] %s\n", sizeNote);
        char *const arguments[] = {(char *)binary.fileSystemRepresentation, "rewritten/" TOTK_GAME_DIR, "boot", NULL};
        execv(arguments[0], arguments);
        fprintf(stderr, "[launcher] execv failed: %s\n", strerror(errno));
        show_error([NSString stringWithFormat:@"Starting the game failed. See %@.", log]);
        return 1;
    }
}
