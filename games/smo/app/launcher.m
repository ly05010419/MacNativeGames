/* Super Mario Odyssey launcher: direct game boot with no pre-launch modal (like TotK).
   In-game settings are controlled via keys 1-5.
*/
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

static void show_error(NSString *detail) {
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"Super Mario Odyssey konnte nicht gestartet werden";
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

static uint64_t pid_start(pid_t pid) {
    struct proc_bsdinfo bi;
    if (pid <= 0 || proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi || bi.pbi_status == SZOMB) return 0;
    return bi.pbi_start_tvsec;
}

static pid_t running_game(NSString *slot) {
    int fd = open([slot stringByAppendingPathComponent:@"lock"].fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    if (!held) return 0;
    NSDictionary *h = kv_read([slot stringByAppendingPathComponent:@"holder"]);
    const char *requestOwner = getenv("TOTK_SLOT_OWNER");
    if (![h[@"owner"] isEqualToString:@"user"] &&
        (!requestOwner || !*requestOwner || !strcmp(requestOwner, "user"))) return 0;
    pid_t pid = [h[@"game_pid"] intValue];
    return pid > 0 && pid_start(pid) == (uint64_t)[h[@"game_start"] longLongValue] ? pid : 0;
}

static NSString *work_folder(void) {
    const char *e = getenv("SMO_WORK");
    if (e && *e) return @(e);
    NSString *val = [NSBundle.mainBundle objectForInfoDictionaryKey:@"SMOWorkFolder"];
    if (val && val.length) return val;
    val = [NSBundle.mainBundle objectForInfoDictionaryKey:@"BY3WorkFolder"];
    if (val && val.length) return val;
    NSString *bundleDir = NSBundle.mainBundle.bundlePath.stringByDeletingLastPathComponent;
    NSString *candidate = [bundleDir stringByAppendingPathComponent:@"SMO_build"];
    if ([NSFileManager.defaultManager fileExistsAtPath:candidate]) return candidate;
    candidate = [bundleDir stringByAppendingPathComponent:@"games/smo/SMO_build"];
    if ([NSFileManager.defaultManager fileExistsAtPath:candidate]) return candidate;
    return nil;
}

static BOOL make_dir(NSString *p) {
    return [NSFileManager.defaultManager createDirectoryAtPath:p withIntermediateDirectories:YES attributes:nil error:nil];
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSString *work = work_folder();
        if (!work) {
            show_error(@"Arbeitsordner SMO_build wurde nicht gefunden.");
            return 1;
        }
        NSString *binary = [work stringByAppendingPathComponent:@"build/smo-native"];
        if (access(binary.fileSystemRepresentation, X_OK) != 0) {
            show_error([NSString stringWithFormat:@"Ausführbare Datei %@ nicht gefunden.", binary]);
            return 1;
        }

        NSString *home = NSHomeDirectory();
        NSString *data = [home stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/SMO"];
        NSString *logs = [home stringByAppendingPathComponent:@"Library/Logs/MacNativeGames"];
        NSString *profile = [data stringByAppendingPathComponent:@"profile"], *slot = [data stringByAppendingPathComponent:@"game-slot"];

        if (!make_dir(profile) || !make_dir(slot) || !make_dir(logs)) {
            show_error([NSString stringWithFormat:@"Ordner konnte nicht erstellt werden: %@", data]);
            return 1;
        }

        pid_t running = running_game(slot);
        if (running) {
            [[NSRunningApplication runningApplicationWithProcessIdentifier:running] activateWithOptions:NSApplicationActivateAllWindows];
            return 0;
        }

        if (chdir(work.fileSystemRepresentation) != 0) {
            show_error([NSString stringWithFormat:@"Konnte nicht in %@ wechseln.", work]);
            return 1;
        }

        setenv("DM_PRESENT_HZ", "60", 0);
        setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
        setenv("TOTK_GPU_WIDEN", "1", 1);
        setenv("TOTK_GPU_BLOCKRT", "1", 1);
        setenv("TOTK_GPU_MIDCLEAR", "1", 1);
        setenv("TOTK_GPU_BUDGET_MB", "5000", 1);
        setenv("DM_PRESENT_LINK_FALLBACK", "1", 1);
        setenv("TOTK_SLOT_OWNER", "user", 1);
        setenv("TOTK_SLOT_PURPOSE", "player", 1);
        setenv("TOTK_PROFILE_DIR", profile.fileSystemRepresentation, 1);
        setenv("TOTK_GAME_SLOT_DIR", slot.fileSystemRepresentation, 1);
        setenv("TOTK_ROMFS_PROOF_DIR", data.fileSystemRepresentation, 1);
        setenv("TOTK_SHARED_FONTS", "extracted/firmware/shared-fonts", 1);
        setenv("TOTK_MFX_UPSCALE", "1", 1);
        setenv("TOTK_MFX_OUTPUT", "1", 1);
        setenv("SMO_HUD_RENDER", "1", 1);
        setenv("BY3_HUD_RENDER", "1", 1);

        NSString *preferred = NSLocale.preferredLanguages.firstObject;
        if (preferred) setenv("TOTK_LOCALE", preferred.UTF8String, 1);

        unsetenv("TOTK_HEADLESS");
        {
            NSString *cmds = [data stringByAppendingPathComponent:@"input.txt"];
            FILE *cf = fopen(cmds.fileSystemRepresentation, "w");
            if (cf) fclose(cf);
            setenv("TOTK_INPUT_FILE", cmds.fileSystemRepresentation, 1);
        }

        NSString *log = [logs stringByAppendingPathComponent:@"smo.log"], *prev = [logs stringByAppendingPathComponent:@"smo.previous.log"];
        [NSFileManager.defaultManager removeItemAtPath:prev error:nil];
        [NSFileManager.defaultManager moveItemAtPath:log toPath:prev error:nil];
        FILE *lf = fopen(log.fileSystemRepresentation, "w");
        if (lf) {
            dup2(fileno(lf), STDOUT_FILENO);
            dup2(fileno(lf), STDERR_FILENO);
            fclose(lf);
        }

        char *const args[] = {(char *)binary.fileSystemRepresentation, "rewritten/v130", "boot", NULL};
        execv(args[0], args);

        fprintf(stderr, "[launcher] execv failed: %s\n", strerror(errno));
        show_error([NSString stringWithFormat:@"Starten des Spiels fehlgeschlagen. Siehe %@", log]);
        return 1;
    }
}
