// The launcher inside the packaged app (build_app.py compiles it as Contents/MacOS/TOTK).
// The game itself is Contents/Resources/totk/build/totk-native; everything it writes goes to the player's own folders:
//   ~/Library/Application Support/MacNativeGames/TOTK/   profile (account, saves), shader cache, game slot, RomFS proof (BotW/ for Breath of the Wild, Wonder/ for Super Mario Bros. Wonder)
//   ~/Library/Logs/MacNativeGames/totk.log             this run, only with the debug log on (totk.previous.log: the one before; botw.log, wonder.log for the other games)
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
#include "totk_contrib_upload.h"   // the shader inbox: one question, sent while playing

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





/* The one question about the shader contribution (totk_contrib_upload.h, the same as Tears of the Kingdom's), before the
   game starts, once the file holds TOTK_CONTRIB_STEP new rows and the player has not chosen. */
static void contrib_ask(NSString *path) {
    NSUInteger rows = totk_inbox_should_ask(path);
    if (!rows) return;
    [NSApplication sharedApplication]; [NSApp activateIgnoringOtherApps:YES];
    NSAlert *alert = [NSAlert new];
    alert.messageText = totk_inbox_prompt_title();
    alert.informativeText = totk_inbox_prompt_text();
    for (NSString *answer in totk_inbox_prompt_answers()) [alert addButtonWithTitle:answer];
    NSModalResponse choice = [alert runModal];
    totk_inbox_answer(path, choice == NSAlertFirstButtonReturn ? TOTK_INBOX_AUTO : TOTK_INBOX_NEVER);
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
    // this game's own file (GAME_CONTRIB_FILE, totk_shader_contrib.h), beside Tears of the Kingdom's and Breath of the
    // Wild's in the same folder: its rows go to its own list (user 2026-10-28), not into theirs
    NSString *mine = [folder stringByAppendingPathComponent:@GAME_CONTRIB_FILE];
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
#if defined(TOTK_GAME_V112) || defined(TOTK_GAME_V121) || defined(TOTK_GAME_V143) || defined(WONDER_V121)
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
        NSString *contrib = env_or("TOTK_SHADER_CONTRIB", contrib_file(data));
        setenv("TOTK_SHADER_CONTRIB", contrib.fileSystemRepresentation, 1);
        contrib_ask(contrib);
        totk_inbox_start(contrib, @"mac");   // the player chose "Contribute shaders": sent while playing
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
