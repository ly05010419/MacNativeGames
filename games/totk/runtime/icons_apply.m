/* The settings panel's Apply button icons (Mac, runtime/icons_apply.h). Steps, as the progress bar counts them: 1 the
   builder's icons Python imports fontTools, 2 <mods> is cloned to <mods>-keyboard.next.partial (an empty folder when
   there is none) with the plan of the layout in force beside it (keyboard_icons.h), then make_button_icons.py --style
   plan draws into the copy, one step per 'progress k/n' line it prints. Done: made-from.txt and complete are written
   and the copy is renamed <mods>-keyboard.next, replacing an older one; the launcher takes it at the next start
   (keyboard_mods.h). A failure or a cancel removes the copy and the plan, so nothing half made is ever taken.
   Paths: TOTK_ICONS_DATA (else the folder of TOTK_CONTROLS), TOTK_ICONS_PYTHON (else the builder's icons-venv),
   TOTK_APP_BUNDLE (the launcher sets it). */
#import <Foundation/Foundation.h>
#include <copyfile.h>
#include <os/lock.h>
#include "icons_apply.h"
#include "keyboard_icons.h"
#include "keyboard_mods.h"
#include "game_version.h"

void runtime_keymap_get(KeyMap *out);   /* runtime/input.c */

enum { ICONS_FIRST_STEPS = 2, ICONS_CHECK_SECONDS = 10 };
static dispatch_queue_t queue;          /* the job's steps run on it, one after another */
/* The run's state below, under the lock: the panel reads it on the main thread, which must not wait for a step. */
static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
static IconsApplyState state = IconsApplyIdle;
static int stepDone, stepCount = ICONS_FIRST_STEPS + 6;
static unsigned run;                    /* each start and cancel moves it on: a job whose number is gone stops */
static NSTask *drawing;
static NSString *lastError;
static IconsApplyUpdate listener;

static NSString *data_folder(void) {
    const char *data = getenv("TOTK_ICONS_DATA"), *controls = getenv("TOTK_CONTROLS");
    if (data && *data) return @(data);
    return controls && *controls ? @(controls).stringByDeletingLastPathComponent : nil;
}
static NSString *python_path(void) {
    const char *python = getenv("TOTK_ICONS_PYTHON");
    if (python && *python) return @(python);
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/Builder/icons-venv/bin/python"];
}
static NSString *mods_folder(void) { return [[data_folder() stringByAppendingPathComponent:@"mods"] stringByAppendingPathComponent:@TOTK_GAME_DIR]; }
static NSString *partial_folder(void) { return [mods_folder() stringByAppendingString:@"-keyboard.next.partial"]; }
static NSString *plan_file(void) { return [mods_folder() stringByAppendingString:@"-keyboard.plan.json"]; }

static void tell(void) {   /* under the lock */
    IconsApplyUpdate update = listener; IconsApplyState s = state; int done = stepDone, count = stepCount;
    NSString *detail = lastError;
    if (update) dispatch_async(dispatch_get_main_queue(), ^{ update(s, done, count, detail); });
}
static void clean(void) {   /* on queue */
    [NSFileManager.defaultManager removeItemAtPath:partial_folder() error:nil];
    [NSFileManager.defaultManager removeItemAtPath:plan_file() error:nil];
}
/* Step `done` of the job reached; NO once the job was cancelled (its partial copy is then the queue's to remove). */
static BOOL step(unsigned job, int done) {
    os_unfair_lock_lock(&lock);
    BOOL now = run == job;
    if (now) { stepDone = done; tell(); }
    os_unfair_lock_unlock(&lock);
    return now;
}
static void finish(unsigned job, IconsApplyState end, NSString *detail) {   /* on queue; cleans before it tells */
    os_unfair_lock_lock(&lock);
    BOOL now = run == job;
    os_unfair_lock_unlock(&lock);
    if (end != IconsApplyDone || !now) clean();
    os_unfair_lock_lock(&lock);
    if (run == job) { drawing = nil; state = end; lastError = detail; if (end == IconsApplyDone) stepDone = stepCount; tell(); }
    os_unfair_lock_unlock(&lock);
}

static BOOL has_fonttools(NSString *python) {
    if (![NSFileManager.defaultManager isExecutableFileAtPath:python]) return NO;
    NSTask *check = [NSTask new];
    check.executableURL = [NSURL fileURLWithPath:python];
    check.arguments = @[@"-I", @"-c", @"import fontTools"];
    check.standardOutput = check.standardError = [NSFileHandle fileHandleWithNullDevice];
    if (![check launchAndReturnError:nil]) return NO;
    NSDate *limit = [NSDate dateWithTimeIntervalSinceNow:ICONS_CHECK_SECONDS];   /* a Python that hangs is no Python */
    while (check.running && limit.timeIntervalSinceNow > 0) usleep(20000);
    if (check.running) { [check terminate]; [check waitUntilExit]; return NO; }
    return check.terminationStatus == 0;
}

/* A clone of a link is a link, so the drawing would write through it into the game's own mods while it runs. */
static BOOL holds_links(NSString *folder) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSDictionary *top = [files attributesOfItemAtPath:folder error:nil];
    if (!top) return NO;   /* none: the drawing starts from an empty folder */
    if ([top.fileType isEqual:NSFileTypeSymbolicLink]) return YES;
    for (NSURL *url in [files enumeratorAtURL:[NSURL fileURLWithPath:folder] includingPropertiesForKeys:@[NSURLIsSymbolicLinkKey]
                                      options:0 errorHandler:nil]) {
        NSNumber *link = nil;
        if ([url getResourceValue:&link forKey:NSURLIsSymbolicLinkKey error:nil] && link.boolValue) return YES;
    }
    return NO;
}

/* Steps 1 and 2, then the drawing; on queue. */
static void begin(unsigned job) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *python = python_path(), *mods = mods_folder(), *partial = partial_folder();
    const char *bundle = getenv("TOTK_APP_BUNDLE");
    NSString *script = bundle ? [@(bundle) stringByAppendingPathComponent:@"Contents/Resources/totk/scripts/make_button_icons.py"] : nil;
    if (!has_fonttools(python)) { finish(job, IconsApplyNoTools, nil); return; }
    if (!data_folder() || !script || ![files fileExistsAtPath:script]) { finish(job, IconsApplyFailed, @"this app has no icon scripts: update it"); return; }
    if (!step(job, 1)) return;

    clean();
    if (holds_links(mods)) { finish(job, IconsApplyFailed, @"the game's mods folder holds links: put the files themselves there"); return; }
    [files createDirectoryAtPath:partial.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    BOOL copied = [files fileExistsAtPath:mods]
        ? copyfile(mods.fileSystemRepresentation, partial.fileSystemRepresentation, NULL,
                   COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_CLONE) == 0
        : [files createDirectoryAtPath:partial withIntermediateDirectories:NO attributes:nil error:nil];
    KeyMap map; runtime_keymap_get(&map);
    static char plan[16384];
    int written = keyboard_icons_plan(&map, plan, sizeof plan);
    if (!copied || written < 0 || ![[NSData dataWithBytes:plan length:(NSUInteger)written] writeToFile:plan_file() atomically:YES]) {
        finish(job, IconsApplyFailed, @"could not copy the game's mods"); return;
    }
    NSString *source = keyboard_mods_source(mods);
    if (!step(job, 2)) { clean(); return; }

    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:python];
    /* -I without its -P: the script imports its neighbours from its own folder. -B: no __pycache__ in the signed app. */
    task.arguments = @[@"-E", @"-s", @"-B", script, partial, @"--app", @(bundle), @"--version", @TOTK_GAME_DIR, @"--style", @"plan",
                       @"--plan", plan_file(), @"--replace", @"--progress"];
    task.qualityOfService = NSQualityOfServiceBackground;   /* the game keeps the cores it needs */
    NSPipe *out = [NSPipe pipe], *err = [NSPipe pipe];
    task.standardOutput = out; task.standardError = err;
    NSMutableData *errors = [NSMutableData data];
    NSMutableString *pending = [NSMutableString string];
    out.fileHandleForReading.readabilityHandler = ^(NSFileHandle *handle) {
        NSData *data = handle.availableData;
        NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
        dispatch_async(queue, ^{
            [pending appendString:text];
            NSRange end;
            while ((end = [pending rangeOfString:@"\n"]).location != NSNotFound) {
                NSString *line = [pending substringToIndex:end.location];
                [pending deleteCharactersInRange:NSMakeRange(0, end.location + 1)];
                int k, n;
                if (sscanf(line.UTF8String, "progress %d/%d", &k, &n) == 2 && n > 0 && k <= n) {
                    os_unfair_lock_lock(&lock);
                    if (drawing == task) { stepCount = ICONS_FIRST_STEPS + n; stepDone = ICONS_FIRST_STEPS + k; tell(); }
                    os_unfair_lock_unlock(&lock);
                }
            }
        });
    };
    err.fileHandleForReading.readabilityHandler = ^(NSFileHandle *handle) {
        NSData *data = handle.availableData;
        dispatch_async(queue, ^{ [errors appendData:data]; });
    };
    task.terminationHandler = ^(NSTask *ended) {
        NSData *rest = [out.fileHandleForReading readDataToEndOfFile];
        out.fileHandleForReading.readabilityHandler = nil; err.fileHandleForReading.readabilityHandler = nil;
        NSData *errRest = [err.fileHandleForReading readDataToEndOfFile];
        (void)rest;
        dispatch_async(queue, ^{
            os_unfair_lock_lock(&lock);
            BOOL cancelled = drawing != ended;
            os_unfair_lock_unlock(&lock);
            if (cancelled) return;   /* the cancel removes the copy */
            [errors appendData:errRest];
            if (ended.terminationStatus != 0) {
                NSString *text = [[NSString alloc] initWithData:errors encoding:NSUTF8StringEncoding] ?: @"";
                NSArray *lines = [[text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
                                  componentsSeparatedByString:@"\n"];
                fprintf(stderr, "[totk] button icons: make_button_icons.py failed:\n%s\n", text.UTF8String);
                finish(job, IconsApplyFailed, [lines.lastObject length] ? lines.lastObject : @"the drawing stopped");
                return;
            }
            NSFileManager *files = NSFileManager.defaultManager;
            NSString *next = [mods stringByAppendingString:@"-keyboard.next"];
            BOOL done = [[source stringByAppendingString:@"\n"] writeToFile:[partial stringByAppendingPathComponent:KEYBOARD_MODS_MADE_FROM]
                                                                 atomically:YES encoding:NSUTF8StringEncoding error:nil] &&
                        [files createFileAtPath:[partial stringByAppendingPathComponent:@"complete"] contents:[NSData data] attributes:nil];
            [files removeItemAtPath:plan_file() error:nil];
            [files removeItemAtPath:next error:nil];
            if (!done || ![files moveItemAtPath:partial toPath:next error:nil]) { finish(job, IconsApplyFailed, @"could not stage the icons"); return; }
            fprintf(stderr, "[totk] button icons: staged in %s for the next start\n", next.UTF8String);
            finish(job, IconsApplyDone, nil);
        });
    };
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) { finish(job, IconsApplyFailed, error.localizedDescription); return; }
    os_unfair_lock_lock(&lock);
    BOOL now = run == job;
    if (now) drawing = task;
    os_unfair_lock_unlock(&lock);
    if (!now) { [task terminate]; [task waitUntilExit]; clean(); }   /* cancelled while it was being started */
}

BOOL icons_apply_start(IconsApplyUpdate update) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("totk.icons-apply", DISPATCH_QUEUE_SERIAL); atexit(icons_apply_cancel); });
    os_unfair_lock_lock(&lock);
    BOOL started = state != IconsApplyRunning;
    unsigned job = 0;
    if (started) {
        job = ++run; listener = update; state = IconsApplyRunning; lastError = nil;
        stepDone = 0; stepCount = ICONS_FIRST_STEPS + 6;
    }
    os_unfair_lock_unlock(&lock);
    if (started) dispatch_async(queue, ^{ begin(job); });
    return started;
}

IconsApplyState icons_apply_state(int *step, int *steps) {
    os_unfair_lock_lock(&lock);
    IconsApplyState s = state;
    if (step) *step = stepDone;
    if (steps) *steps = stepCount;
    os_unfair_lock_unlock(&lock);
    return s;
}

/* Does not wait for a step in progress (the tools check, the copy): that step finds its job gone and removes what it
   made. A drawing is stopped and its copy removed here, since a window's close ends the process right after
   (runtime/frontend.m); once it runs, nothing else writes the copy. */
void icons_apply_cancel(void) {
    os_unfair_lock_lock(&lock);
    BOOL running = state == IconsApplyRunning;
    NSTask *task = drawing;
    if (running) { run++; drawing = nil; state = IconsApplyIdle; listener = nil; }
    os_unfair_lock_unlock(&lock);
    if (!running) return;
    if (!task) return;
    if (task.running) { [task terminate]; [task waitUntilExit]; }
    clean();
}

int icons_apply_stale(void) {
    return data_folder() ? keyboard_mods_stale(mods_folder()) : 0;
}
