// Cloud saves on the iPhone: the copies to and from the iCloud Drive folder the player picked (dm/host/save_sync.h; the folder is a bookmark, CloudSaves.m).
// They go through NSFileCoordinator: a file iCloud has only listed is downloaded before the read, and neither copy crosses iCloud's own writes of the file.
// These replace the host's plain-copy defaults (weak, in save_sync.c) at link time; the host packs, unpacks and decides, this moves the one file.
#import <Foundation/Foundation.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "save_sync.h"

int front_save_sync_fetch(const char *cloud, const char *local) {
    @autoreleasepool {
        NSFileManager *fm = [NSFileManager new];
        NSURL *src = [NSURL fileURLWithPath:@(cloud)], *dst = [NSURL fileURLWithPath:@(local)];
        // Not there, not even as the placeholder iCloud lists for a file it has not downloaded (.<name>.icloud): no save in the cloud yet
        NSString *placeholder = [src.URLByDeletingLastPathComponent URLByAppendingPathComponent:[NSString stringWithFormat:@".%@.icloud", src.lastPathComponent]].path;
        if (![fm fileExistsAtPath:src.path] && ![fm fileExistsAtPath:placeholder]) return 0;
        [fm startDownloadingUbiquitousItemAtURL:src error:nil];   // asked for now; the coordinated read waits for it (save_sync.c gives it a time limit)
        __block int r = -1;
        NSError *error = nil;
        [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateReadingItemAtURL:src options:0 error:&error byAccessor:^(NSURL *u) {
            NSError *e = nil;
            [fm removeItemAtURL:dst error:nil];
            if ([fm copyItemAtURL:u toURL:dst error:&e]) r = 1;
            else if ([e.domain isEqualToString:NSCocoaErrorDomain] && e.code == NSFileReadNoSuchFileError) r = 0;
            else fprintf(stderr, "[dm] [RED] save sync: copying %s from the cloud failed: %s\n", cloud, e.localizedDescription.UTF8String ?: "no reason given");
        }];
        if (error) fprintf(stderr, "[dm] [RED] save sync: reading %s from the cloud failed: %s\n", cloud, error.localizedDescription.UTF8String ?: "no reason given");
        return r;
    }
}

int front_save_sync_store(const char *local, const char *cloud) {
    @autoreleasepool {
        NSFileManager *fm = [NSFileManager new];
        NSURL *dst = [NSURL fileURLWithPath:@(cloud)];
        NSString *src = @(local);
        __block int r = 0;
        NSError *error = nil;
        [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateWritingItemAtURL:dst options:NSFileCoordinatorWritingForReplacing error:&error byAccessor:^(NSURL *u) {
            // Beside it under a .nosync name (iCloud Drive does not upload those), then renamed over it: iCloud sees one whole new file
            NSString *part = [u.path stringByAppendingFormat:@".%d.nosync", (int)getpid()];
            NSError *e = nil;
            [fm removeItemAtPath:part error:nil];
            if (![fm copyItemAtPath:src toPath:part error:&e]) {
                fprintf(stderr, "[dm] [RED] save sync: writing beside %s failed: %s\n", cloud, e.localizedDescription.UTF8String ?: "no reason given");
                return;
            }
            if (rename(part.fileSystemRepresentation, u.fileSystemRepresentation)) {
                fprintf(stderr, "[dm] [RED] save sync: replacing %s failed (errno=%d)\n", cloud, errno);
                [fm removeItemAtPath:part error:nil];
                return;
            }
            r = 1;
        }];
        if (error) fprintf(stderr, "[dm] [RED] save sync: writing %s to the cloud failed: %s\n", cloud, error.localizedDescription.UTF8String ?: "no reason given");
        return r;
    }
}
