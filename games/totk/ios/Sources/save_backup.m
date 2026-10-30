/* Keeps the game's save data in a folder the player picks in iCloud Drive (or anywhere in Files), so it outlives the
   app: deleting the app deletes Documents, not that folder. The folder is reached through a security-scoped bookmark
   stored in NSUserDefaults; a fresh install has no bookmark, so the app asks once (before the game thread starts, so a
   restore cannot race the game's own save loading). "Later" is remembered; the Settings pane has a switch to choose the
   folder again. Every copy is best-effort: a failure only logs, never blocks play.

   Off unless the player turns the Settings switch on (TOTK_BACKUP=1): until then nothing is shown and nothing is
   copied. With it on and no folder stored, the folder picker appears once; the folder inside it is named after the
   game, so a shared iCloud Drive folder stays readable. */
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "runtime.h"
#include "save_backup.h"
#include "totk_ui_lang.h"

static NSString *const kBookmark = @"TOTK_BACKUP_BOOKMARK";
static NSString *const kDeclined = @"TOTK_BACKUP_DECLINED";   // "Later" was chosen: do not ask again on every launch
static NSString *const kReconfigure = @"TOTK_BACKUP_RECONFIGURE";   // a Settings switch: forget the folder and ask again
static NSString *const kImportRequest = @"TOTK_SAVE_IMPORT";   // a Settings switch: pick a save folder on the next launch
static NSString *const kBeforeImport = @"save-before-import";   // the folder the import replaces, kept for one mistake
/* The folder this game keeps inside the picked one: the game's own name, so a shared iCloud Drive folder reads clearly. */
static NSString *game_folder(void) {
    NSString *name = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"];
    return name.length ? name : @"TOTK";
}
static NSString *const kDirectories[] = {@"save", @"game-slot"};
static const char *const kDirectoryEnvs[] = {"TOTK_PROFILE_DIR", "TOTK_GAME_SLOT_DIR"};   // main.m sets both

@interface TOTKSaveBackup () <UIDocumentPickerDelegate>
@property (nonatomic, copy) void (^pickCompletion)(BOOL);
@property (nonatomic, copy) void (^importCompletion)(void);   // a picked folder is being imported, not adopted as the backup
@property (nonatomic, weak) UIViewController *importHost;
- (void)prepareFrom:(UIViewController *)host completion:(void (^)(void))ready;
- (void)explain:(UIViewController *)host choose:(void (^)(void))choose later:(void (^)(void))later;
- (void)chooseFrom:(UIViewController *)host completion:(void (^)(BOOL))completion;
- (BOOL)adoptBookmark:(NSData *)bookmark;
- (BOOL)adopt:(NSURL *)url;
- (void)restoreIfFresh;
- (void)restoreIfFreshWithCompletion:(void (^)(void))completion;
- (void)background;
- (void)syncNow;
- (void)syncNowWithCompletion:(void (^)(void))completion;
- (NSUInteger)mirror:(NSURL *)from to:(NSURL *)to overwrite:(BOOL)overwrite;
- (BOOL)sameContents:(NSURL *)from as:(NSURL *)to;
- (NSURL *)localDirectory:(NSUInteger)i;
@end

@implementation TOTKSaveBackup {
    NSURL *root;                 // <picked folder>/TOTK, scoped for the app's lifetime
    dispatch_queue_t queue;
    volatile int stopping;       // the background task ran out: abandon the copy between files
}

+ (instancetype)shared {
    static TOTKSaveBackup *backup;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ backup = [TOTKSaveBackup new]; });
    return backup;
}

+ (BOOL)enabled {
    /* Off by default: the picker must only appear after the player asked for backup in Settings. */
    const char *value = getenv("TOTK_BACKUP");
    return value && strcmp(value, "0") != 0;
}

+ (void)prepareFrom:(UIViewController *)host completion:(void (^)(void))ready {
    [[self shared] prepareFrom:host completion:ready];
}

+ (void)sync { [[self shared] syncNow]; }

- (instancetype)init {
    if ((self = [super init])) queue = dispatch_queue_create("totk.save-backup", DISPATCH_QUEUE_SERIAL);
    return self;
}

- (void)prepareFrom:(UIViewController *)host completion:(void (^)(void))ready {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    /* A one-shot request from Settings: clear it first, so a launch that dies mid-way cannot loop the picker. The
       import runs before the game thread; the normal backup flow follows it. */
    if ([defaults boolForKey:kImportRequest]) {
        [defaults setBool:NO forKey:kImportRequest];
        [self importFrom:host completion:^{ [self prepareFrom:host completion:ready]; }];
        return;
    }
    if (!TOTKSaveBackup.enabled) {
        /* Turning backup off also forgets an earlier "Later", so turning it on again asks for a folder. */
        [defaults removeObjectForKey:kDeclined];
        [defaults setBool:NO forKey:kReconfigure];
        ready();
        return;
    }
    if ([defaults boolForKey:kReconfigure]) {
        [defaults removeObjectForKey:kBookmark];
        [defaults removeObjectForKey:kDeclined];
        [defaults setBool:NO forKey:kReconfigure];
        fprintf(stderr, "[totk-save] backup folder will be chosen again\n");
    }
    __block BOOL started = NO;
    void (^done)(void) = ^{
        if (started) return;
        started = YES;
        ready();
        [self syncNow];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(background)
                                                   name:UIApplicationDidEnterBackgroundNotification object:nil];
        [NSTimer scheduledTimerWithTimeInterval:300 repeats:YES block:^(NSTimer *timer) {
            (void)timer; [self syncNow];
        }];
    };
    NSData *bookmark = [defaults dataForKey:kBookmark];
    if (bookmark && [self adoptBookmark:bookmark]) {
        [defaults removeObjectForKey:kDeclined];
        fprintf(stderr, "[totk-save] backup folder %s\n", root.path.UTF8String);
        [self restoreIfFreshWithCompletion:done];
        return;
    }
    /* A bookmark that no longer resolves is a broken backup, not a player who said "later": ask again once, then forget
       the dead bookmark — otherwise it clears "Later" on every launch and the prompt can never be dismissed. */
    if (bookmark) { [defaults removeObjectForKey:kBookmark]; [defaults removeObjectForKey:kDeclined]; }
    if ([defaults boolForKey:kDeclined]) {
        fprintf(stderr, "[totk-save] backup folder not chosen earlier; starting without one (choose it again in Settings)\n");
        done();
        return;
    }
    [self explain:host choose:^{
        [self chooseFrom:host completion:^(BOOL picked) {
            if (picked) [defaults removeObjectForKey:kDeclined];
            else [defaults setBool:YES forKey:kDeclined];
            if (picked) [self restoreIfFreshWithCompletion:done]; else done();
        }];
    } later:done];
}

/* One short explanation before the system folder picker; "Later" is remembered, so the next launch starts the game
   without asking again. The Settings pane's "choose the backup folder again" switch clears that choice. */
- (void)explain:(UIViewController *)host choose:(void (^)(void))choose later:(void (^)(void))later {
    if (!host) { later(); return; }
    __block BOOL answered = NO;
    void (^finish)(BOOL) = ^(BOOL chose) {
        if (answered) return;
        answered = YES;
        if (chose) choose(); else later();
    };
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@(TOTK_UI("存档自动备份", "Save backup"))
                                                                   message:@(TOTK_UI(
        "选择一个文件夹（例如「文件」App 里），存档会自动备份过去。以后删掉这个 App，存档也还在；重装后选中同一个文件夹即可恢复。",
        "Pick a folder (for example in the Files app); saves are backed up there automatically. They survive deleting this app, and a reinstall restores them from the same folder."))
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("选择文件夹", "Choose folder")) style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        (void)action;
        if (answered) return;
        answered = YES;
        /* Presenting while the alert is still dismissing is refused; one run-loop turn after it is gone is enough. */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.4 * NSEC_PER_SEC)), dispatch_get_main_queue(), choose);
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("以后再说", "Later")) style:UIAlertActionStyleCancel
                                            handler:^(UIAlertAction *action) {
        (void)action;
        [NSUserDefaults.standardUserDefaults setBool:YES forKey:kDeclined];
        finish(NO);
    }]];
    [host presentViewController:alert animated:YES completion:^{
        if (host.presentedViewController != alert) finish(NO);   // another presenter won: do not leave the game waiting
    }];
    /* Nobody answered (unattended launch, a scene torn down): start the game without a folder rather than block boot. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(300 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (answered) return;
        fprintf(stderr, "[totk-save] backup prompt did not answer in 5 minutes; starting without a backup folder\n");
        [alert dismissViewControllerAnimated:YES completion:nil];
        finish(NO);
    });
}

- (void)chooseFrom:(UIViewController *)host completion:(void (^)(BOOL))completion {
    if (!host) { completion(NO); return; }
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
    picker.allowsMultipleSelection = NO;
    picker.delegate = self;
    __block BOOL answered = NO;
    void (^finish)(BOOL) = ^(BOOL ok) {
        if (answered) return;
        answered = YES;
        self.pickCompletion = nil;
        completion(ok);
    };
    self.pickCompletion = finish;
    [host presentViewController:picker animated:YES completion:^{
        if (host.presentedViewController != picker) finish(NO);
    }];
    /* A picker that never calls back (scene torn down, app switched away) must not keep the game from starting. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(300 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (answered) return;
        fprintf(stderr, "[totk-save] folder picker did not answer in 5 minutes; starting without a backup folder\n");
        [picker dismissViewControllerAnimated:YES completion:nil];
        finish(NO);
    });
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    NSURL *picked = urls.firstObject;
    if (self.importCompletion) {
        void (^done)(void) = self.importCompletion; self.importCompletion = nil;
        /* The confirmation alert is presented from the same host: wait until the picker is gone, or UIKit refuses it. */
        [controller dismissViewControllerAnimated:YES completion:^{
            if (picked) [self importPicked:picked completion:done]; else done();
        }];
        return;
    }
    BOOL ok = picked && [self adopt:picked];
    if (ok) {
        NSError *error = nil;
        NSData *bookmark = [picked bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark
                                includingResourceValuesForKeys:nil relativeToURL:nil error:&error];
        if (bookmark) [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:kBookmark];
        else fprintf(stderr, "[totk-save] bookmark failed: %s\n", error.localizedDescription.UTF8String);
        fprintf(stderr, "[totk-save] backup folder %s\n", root.path.UTF8String);
    }
    void (^completion)(BOOL) = self.pickCompletion; self.pickCompletion = nil;
    if (completion) completion(ok);
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    (void)controller;
    if (self.importCompletion) {
        void (^done)(void) = self.importCompletion; self.importCompletion = nil;
        fprintf(stderr, "[totk-save] import folder not chosen\n");
        done();
        return;
    }
    void (^completion)(BOOL) = self.pickCompletion; self.pickCompletion = nil;
    fprintf(stderr, "[totk-save] backup folder not chosen; saves stay in Documents\n");
    if (completion) completion(NO);
}

- (BOOL)adoptBookmark:(NSData *)bookmark {
    BOOL stale = NO; NSError *error = nil;
    NSURL *url = [NSURL URLByResolvingBookmarkData:bookmark options:NSURLBookmarkResolutionWithoutUI
                                      relativeToURL:nil bookmarkDataIsStale:&stale error:&error];
    if (!url) { fprintf(stderr, "[totk-save] backup folder unavailable: %s\n", error.localizedDescription.UTF8String); return NO; }
    if (![self adopt:url]) return NO;
    if (stale) {
        NSData *fresh = [url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark
                      includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
        if (fresh) [NSUserDefaults.standardUserDefaults setObject:fresh forKey:kBookmark];
    }
    return YES;
}

/* The folder the player picked may be TOTK itself (a reinstall) or its parent; both land on the same place. */
- (BOOL)adopt:(NSURL *)url {
    if (![url startAccessingSecurityScopedResource]) fprintf(stderr, "[totk-save] no security scope for %s\n", url.path.UTF8String);
    root = [url.lastPathComponent isEqualToString:game_folder()] ? url : [url URLByAppendingPathComponent:game_folder() isDirectory:YES];
    NSError *error = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:&error]) {
        fprintf(stderr, "[totk-save] cannot create %s: %s\n", root.path.UTF8String, error.localizedDescription.UTF8String);
        root = nil;
        return NO;
    }
    return YES;
}

/* The one-shot import the Settings switch asks for: pick a folder, confirm, keep what is replaced, copy its save in.
   Nothing is removed before a complete copy of the import sits in a temporary folder next to the live one, and the
   replaced save is kept under Documents/save-before-import. */
- (void)importFrom:(UIViewController *)host completion:(void (^)(void))done {
    if (!host) { done(); return; }
    self.importHost = host;
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
    picker.allowsMultipleSelection = NO;
    picker.delegate = self;
    __block BOOL answered = NO;
    void (^finish)(void) = ^{
        if (answered) return;
        answered = YES;
        self.importCompletion = nil;
        done();
    };
    self.importCompletion = finish;
    [host presentViewController:picker animated:YES completion:^{
        if (host.presentedViewController != picker) finish();
    }];
    /* Like the backup folder: a picker that never answers must not keep the game from starting. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(300 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (answered) return;
        fprintf(stderr, "[totk-save] import picker did not answer in 5 minutes; starting with the current save\n");
        [picker dismissViewControllerAnimated:YES completion:nil];
        finish();
    });
}

/* The picked folder is either the game's own folder or a parent that holds it; the backup folder has the same shape,
   so a backup can be imported the same way. */
- (NSURL *)saveSourceIn:(NSURL *)folder {
    NSURL *named = [folder URLByAppendingPathComponent:game_folder() isDirectory:YES];
    for (NSURL *candidate in @[named, folder])
        if ([NSFileManager.defaultManager fileExistsAtPath:[[candidate URLByAppendingPathComponent:kDirectories[0]] path]])
            return candidate;
    return nil;
}

- (void)importPicked:(NSURL *)picked completion:(void (^)(void))done {
    BOOL scoped = [picked startAccessingSecurityScopedResource];
    NSURL *source = [self saveSourceIn:picked];
    if (!source) {
        fprintf(stderr, "[totk-save] %s holds no save; import skipped\n", picked.path.UTF8String);
        if (scoped) [picked stopAccessingSecurityScopedResource];
        done();
        return;
    }
    UIViewController *host = self.importHost;
    if (!host) { if (scoped) [picked stopAccessingSecurityScopedResource]; done(); return; }
    NSString *message = [NSString stringWithFormat:@"%@\n\n%@", picked.path,
        @(TOTK_UI("导入后会替换当前存档；当前存档会先备份到 App 的 Documents/save-before-import。",
                  "Importing replaces the current save; it is first kept in the app's Documents/save-before-import."))];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@(TOTK_UI("导入存档？", "Import this save?"))
                                                                  message:message
                                                           preferredStyle:UIAlertControllerStyleAlert];
    __block BOOL answered = NO;   /* the player answered: the timeout must stop asking */
    __block BOOL finished = NO;
    void (^finish)(void) = ^{
        if (finished) return;
        finished = YES;
        done();
    };
    [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("取消", "Cancel")) style:UIAlertActionStyleCancel
                                            handler:^(UIAlertAction *action) {
        (void)action;
        answered = YES;
        if (scoped) [picked stopAccessingSecurityScopedResource];
        finish();
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("导入", "Import")) style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        (void)action;
        answered = YES;   /* the copy may take a while; the game must not start on a half-written save */
        [self copyImport:source from:picked scoped:scoped completion:finish];
    }]];
    [host presentViewController:alert animated:YES completion:^{
        if (host.presentedViewController != alert) {   /* another presenter won: do not leave the game waiting */
            fprintf(stderr, "[totk-save] import prompt could not be shown; skipped\n");
            answered = YES;
            if (scoped) [picked stopAccessingSecurityScopedResource];
            finish();
        }
    }];
    /* A prompt nobody answers (a scene torn down while it is up) must not keep the game from starting. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(300 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (answered) return;
        fprintf(stderr, "[totk-save] import prompt did not answer in 5 minutes; skipped\n");
        answered = YES;
        [alert dismissViewControllerAnimated:YES completion:nil];
        if (scoped) [picked stopAccessingSecurityScopedResource];
        finish();
    });
}

- (void)copyImport:(NSURL *)source from:(NSURL *)picked scoped:(BOOL)scoped completion:(void (^)(void))done {
    dispatch_async(queue, ^{
        self->stopping = 0;
        NSFileManager *files = NSFileManager.defaultManager;
        /* 1. Keep what is going to be replaced: a partial copy of it is as bad as none, so the import stops while the
           live save is still in place unless every file of it was copied. */
        NSURL *keep = [[self documents] URLByAppendingPathComponent:kBeforeImport isDirectory:YES];
        [files removeItemAtURL:keep error:nil];
        BOOL keptInFull = YES;
        for (NSUInteger i = 0; i < sizeof kDirectories / sizeof *kDirectories; i++) {
            NSURL *live = [self localDirectory:i];
            NSURL *copy = [keep URLByAppendingPathComponent:kDirectories[i] isDirectory:YES];
            BOOL liveHasFiles = [[files contentsOfDirectoryAtPath:live.path error:nil] count] > 0;
            [self mirror:live to:copy overwrite:YES];
            if (liveHasFiles && ![self sameContents:live as:copy]) keptInFull = NO;
        }
        if (!keptInFull) {
            fprintf(stderr, "[totk-save] import aborted: the current save could not be kept in full\n");
            if (scoped) [picked stopAccessingSecurityScopedResource];
            dispatch_async(dispatch_get_main_queue(), done);
            return;
        }
        /* 2. Copy each folder beside the live one, then swap: the live save is only removed once the import is complete. */
        NSUInteger copied = 0;
        for (NSUInteger i = 0; i < sizeof kDirectories / sizeof *kDirectories; i++) {
            NSURL *from = [source URLByAppendingPathComponent:kDirectories[i] isDirectory:YES];
            if (![files fileExistsAtPath:from.path]) continue;
            NSURL *to = [self localDirectory:i];
            NSURL *temporary = [to URLByAppendingPathExtension:@"import-tmp"];
            [files removeItemAtURL:temporary error:nil];
            NSUInteger n = [self mirror:from to:temporary overwrite:YES];
            if (!n || ![self sameContents:from as:temporary]) {
                fprintf(stderr, "[totk-save] import of %s is incomplete; the live save is untouched\n", kDirectories[i].UTF8String);
                [files removeItemAtURL:temporary error:nil];
                continue;
            }
            /* Swap in place: replaceItemAtURL keeps the old folder until the new one is moved over, so a failure here
               leaves the live save readable instead of removing it first (the old order did, 2026-10-26). */
            NSError *error = nil;
            BOOL placed;
            if ([files fileExistsAtPath:to.path])
                placed = [files replaceItemAtURL:to withItemAtURL:temporary backupItemName:nil options:0
                               resultingItemURL:NULL error:&error];
            else
                placed = [files moveItemAtURL:temporary toURL:to error:&error];
            if (!placed) {
                fprintf(stderr, "[totk-save] import swap failed: %s (the live save is untouched; the copy stays in %s)\n",
                        error.localizedDescription.UTF8String, temporary.path.UTF8String);
                if (scoped) [picked stopAccessingSecurityScopedResource];
                dispatch_async(dispatch_get_main_queue(), done);
                return;
            }
            copied += n;
        }
        if (scoped) [picked stopAccessingSecurityScopedResource];
        fprintf(stderr, "[totk-save] imported %lu entries from %s; the replaced save is in %s\n",
                (unsigned long)copied, picked.path.UTF8String, keep.path.UTF8String);
        dispatch_async(dispatch_get_main_queue(), done);
    });
}

- (NSURL *)documents {
    NSString *path = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    return [NSURL fileURLWithPath:path isDirectory:YES];
}

/* The game's own directories (main.m exported them): the launcher may point them elsewhere, so backup follows the env. */
- (NSURL *)localDirectory:(NSUInteger)i {
    const char *path = getenv(kDirectoryEnvs[i]);
    if (path && *path) return [NSURL fileURLWithPath:@(path) isDirectory:YES];
    return [[self documents] URLByAppendingPathComponent:kDirectories[i] isDirectory:YES];
}

/* A reinstall has empty Documents folders: copy the backup back before the game starts. Never overwrites anything.
   Off the main thread: a file iCloud has not downloaded yet can take a while, and launch must not watch-dog. */
- (void)restoreIfFreshWithCompletion:(void (^)(void))completion {
    if (!root) { completion(); return; }
    dispatch_async(queue, ^{
        self->stopping = 0;
        [self restoreIfFresh];
        dispatch_async(dispatch_get_main_queue(), completion);
    });
}

- (void)restoreIfFresh {
    if (!root) return;
    for (NSUInteger i = 0; i < sizeof kDirectories / sizeof *kDirectories; i++) {
        NSURL *local = [self localDirectory:i];
        NSArray *existing = [NSFileManager.defaultManager contentsOfDirectoryAtURL:local includingPropertiesForKeys:nil options:0 error:nil];
        if (existing.count) continue;
        NSURL *saved = [root URLByAppendingPathComponent:local.lastPathComponent isDirectory:YES];
        if (![NSFileManager.defaultManager fileExistsAtPath:saved.path]) continue;
        NSUInteger copied = [self mirror:saved to:local overwrite:NO];
        fprintf(stderr, "[totk-save] restored %lu entries of %s from %s\n", (unsigned long)copied, local.lastPathComponent.UTF8String, root.path.UTF8String);
    }
}

- (void)background {
    __block UIBackgroundTaskIdentifier task = [UIApplication.sharedApplication beginBackgroundTaskWithExpirationHandler:^{
        self->stopping = 1;
        [UIApplication.sharedApplication endBackgroundTask:task];
        task = UIBackgroundTaskInvalid;
    }];
    [self syncNowWithCompletion:^{
        if (task != UIBackgroundTaskInvalid) {
            [UIApplication.sharedApplication endBackgroundTask:task];
            task = UIBackgroundTaskInvalid;
        }
    }];
}

- (void)syncNow { [self syncNowWithCompletion:nil]; }

- (void)syncNowWithCompletion:(void (^)(void))completion {
    if (!root || !TOTKSaveBackup.enabled) { if (completion) completion(); return; }
    NSURL *destination = root;
    dispatch_async(queue, ^{
        self->stopping = 0;
        /* A profile the game is still writing (fs_host keeps it flagged for 3 s after the last change) would be stored
           torn; wait for the save to commit, and skip this pass if it takes longer than the wait. */
        for (int i = 0; i < 100 && runtime_fs_save_busy(); i++) usleep(50000);
        if (runtime_fs_save_busy()) {
            fprintf(stderr, "[totk-save] save still in progress; backing up on the next pass\n");
            if (completion) completion();
            return;
        }
        NSUInteger copied = 0;
        for (NSUInteger i = 0; i < sizeof kDirectories / sizeof *kDirectories; i++) {
            NSURL *from = [self localDirectory:i];
            NSURL *to = [destination URLByAppendingPathComponent:from.lastPathComponent isDirectory:YES];
            copied += [self mirror:from to:to overwrite:YES];
        }
        if (copied) fprintf(stderr, "[totk-save] backed up %lu entries to %s\n", (unsigned long)copied, destination.path.UTF8String);
        if (completion) completion();
    });
}

/* Every file under `from` present under `to` with the same size: a copy that stopped partway (a failure, the background
   task running out) must not be swapped in as a save. */
- (BOOL)sameContents:(NSURL *)from as:(NSURL *)to {
    NSFileManager *files = NSFileManager.defaultManager;
    BOOL directory = NO;
    if (![files fileExistsAtPath:from.path isDirectory:&directory]) return YES;   /* nothing to copy */
    if (!directory) {
        NSDictionary *source = [files attributesOfItemAtPath:from.path error:nil];
        NSDictionary *target = [files attributesOfItemAtPath:to.path error:nil];
        return target && [source[NSFileSize] isEqual:target[NSFileSize]];
    }
    NSArray *items = [files contentsOfDirectoryAtURL:from includingPropertiesForKeys:nil options:0 error:nil] ?: @[];
    if (items.count && ![files fileExistsAtPath:to.path]) return NO;   /* the copy never got as far as making the folder */
    for (NSURL *item in items)
        if (![self sameContents:item as:[to URLByAppendingPathComponent:item.lastPathComponent]]) return NO;
    return YES;
}
- (NSUInteger)mirror:(NSURL *)from to:(NSURL *)to overwrite:(BOOL)overwrite {
    if (self->stopping) return 0;
    NSFileManager *files = NSFileManager.defaultManager;
    BOOL directory = NO;
    if (![files fileExistsAtPath:from.path isDirectory:&directory]) return 0;
    NSError *error = nil;
    if (!directory) {
        /* account.uid is exactly 16 bytes (runtime/account.c); restoring a partial one would make the app refuse to
           boot. Never let a torn source replace a good copy, in either direction. */
        if ([from.lastPathComponent isEqualToString:@"account.uid"]) {
            NSDictionary *source = [files attributesOfItemAtPath:from.path error:nil];
            if ([source[NSFileSize] unsignedLongLongValue] != 16) {
                fprintf(stderr, "[totk-save] skip incomplete %s\n", from.path.UTF8String);
                return 0;
            }
        }
        if (!overwrite && [files fileExistsAtPath:to.path]) return 0;
        if (overwrite) {   // an unchanged file is not copied again every five minutes
            NSDictionary *source = [files attributesOfItemAtPath:from.path error:nil];
            NSDictionary *target = [files attributesOfItemAtPath:to.path error:nil];
            NSDate *sourceDate = source[NSFileModificationDate], *targetDate = target[NSFileModificationDate];
            if (sourceDate && targetDate && [source[NSFileSize] isEqual:target[NSFileSize]] &&
                [sourceDate compare:targetDate] != NSOrderedDescending) return 0;
        }
        /* Copy beside it first: a failed copy must leave the previous backup (or save) in place. */
        NSURL *temporary = [to URLByAppendingPathExtension:@"backup-tmp"];
        [files removeItemAtURL:temporary error:nil];
        if (![files copyItemAtURL:from toURL:temporary error:&error]) {
            fprintf(stderr, "[totk-save] copy %s: %s\n", to.path.UTF8String, error.localizedDescription.UTF8String);
            return 0;
        }
        BOOL ok = [files fileExistsAtPath:to.path]
                ? [files replaceItemAtURL:to withItemAtURL:temporary backupItemName:nil options:0 resultingItemURL:nil error:&error]
                : [files moveItemAtURL:temporary toURL:to error:&error];
        if (!ok) {
            fprintf(stderr, "[totk-save] replace %s: %s\n", to.path.UTF8String, error.localizedDescription.UTF8String);
            [files removeItemAtURL:temporary error:nil];
        }
        return ok ? 1 : 0;
    }
    if (![files createDirectoryAtURL:to withIntermediateDirectories:YES attributes:nil error:&error]) {
        fprintf(stderr, "[totk-save] folder %s: %s\n", to.path.UTF8String, error.localizedDescription.UTF8String);
        return 0;
    }
    NSUInteger copied = 0;
    for (NSURL *item in [files contentsOfDirectoryAtURL:from includingPropertiesForKeys:nil options:0 error:nil] ?: @[])
        copied += [self mirror:item to:[to URLByAppendingPathComponent:item.lastPathComponent] overwrite:overwrite];
    return copied;
}

@end
