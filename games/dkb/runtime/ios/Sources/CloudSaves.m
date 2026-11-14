// See CloudSaves.h.

#import "CloudSaves.h"

static NSString *const kCloudSaves = @"cloudSavesBookmark";

// The folder's security scope, held from the first resolve on: the host writes there after every
// save. Never stopped on purpose, the process exits when the game does.
static NSURL *opened = nil;

static NSData *fresh_bookmark(NSURL *url) {
    // The picker grants a security scope that lasts while it is held: the bookmark is written with
    // the scope on. One written without it reads back on this run and resolves to nothing from the
    // next launch on.
    BOOL scoped = [url startAccessingSecurityScopedResource];
    NSData *bookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
    if (scoped) [url stopAccessingSecurityScopedResource];
    return bookmark;
}

NSString *dm_cloud_saves_path(BOOL *stale) {
    if (stale) *stale = NO;
    NSData *bookmark = [NSUserDefaults.standardUserDefaults dataForKey:kCloudSaves];
    if (bookmark.length == 0) return nil;
    BOOL isStale = NO;
    NSError *error = nil;
    NSURL *url = [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil bookmarkDataIsStale:&isStale error:&error];
    if (!url) {
        // A re-signed build gets a bookmark the system will not resolve for it. Keep the data, the
        // player may install the same signature again, and let the launcher ask for a new pick.
        fprintf(stderr, "boot: the picked cloud-save folder bookmark does not resolve (%s)\n", error.localizedDescription.UTF8String ?: "no reason given");
        if (stale) *stale = YES;
        return nil;
    }
    if (opened && ![opened.path isEqualToString:url.path]) opened = nil;
    if (!opened) [url startAccessingSecurityScopedResource];   // held for the session (see opened)
    opened = url;
    if (isStale) {
        // The system still resolves it but asks for a rewrite. Same signature, so a fresh bookmark sticks.
        NSData *again = fresh_bookmark(url);
        if (again) [NSUserDefaults.standardUserDefaults setObject:again forKey:kCloudSaves];
        if (stale) *stale = YES;
    }
    return url.path;
}

BOOL dm_store_cloud_saves(NSURL *url) {
    NSData *bookmark = fresh_bookmark(url);
    NSError *error = nil;
    NSURL *back = nil;
    if (bookmark) back = [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil bookmarkDataIsStale:nil error:&error];
    if (!bookmark || !back) {
        // A bookmark that cannot come back is not saved as if it were good.
        fprintf(stderr, "[RED] could not bookmark %s: %s\n", url.path.UTF8String ?: "?",
                error.localizedDescription.UTF8String ?: (bookmark ? "the bookmark does not resolve again" : "no bookmark data"));
        return NO;
    }
    [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:kCloudSaves];
    fprintf(stderr, "launcher: cloud-save folder set to %s\n", url.path.UTF8String ?: "?");
    return YES;
}

void dm_clear_cloud_saves(void) {
    [NSUserDefaults.standardUserDefaults removeObjectForKey:kCloudSaves];
}

void dm_cloud_saves_env(void) {
    static BOOL fromPick;   // DM_SAVE_SYNC is the one this set, not an env file's
    if (getenv("DM_SAVE_SYNC") && !fromPick) return;
    BOOL stale = NO;
    NSString *folder = dm_cloud_saves_path(&stale);
    if (stale && !folder) fprintf(stderr, "boot: the cloud-save folder needs to be chosen again (the app is signed differently now); cloud saves are off\n");
    if (folder.length) {
        setenv("DM_SAVE_SYNC", folder.fileSystemRepresentation, 1); fromPick = YES;
        fprintf(stderr, "boot: cloud saves through %s\n", folder.fileSystemRepresentation);
    } else if (fromPick) { unsetenv("DM_SAVE_SYNC"); fromPick = NO; }
}
