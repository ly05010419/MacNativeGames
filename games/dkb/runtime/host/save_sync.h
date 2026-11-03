// Saves through a cloud folder (iCloud Drive), so the Mac app and the iPhone app of one game play on from the same save. Off unless DM_SAVE_SYNC names the folder
// (each app's launcher sets the folder the player chose in its Cloud saves: dm/mac/launcher.c from the Mac settings window, CloudSaves.m on the iPhone; "icloud" is iCloud Drive/nxnative Saves).
// The folder holds one file per game, <folder>/<title ID>/save.dmsync (a folder named after the title ID is taken as the game's own): the committed save (DM_SAVE and DM_SAVE_device) packed into one file. iCloud replaces a file as a
// whole but syncs a folder file by file, and a save that is half one device's and half the other's is worse than either.
// When: the cloud's copy comes in at the game's first save mount (hle_fs.c, before the work directory is cloned from the committed save); the committed save goes out a few
// seconds after a Commit, on a thread of its own (the game never waits), when the iPhone app goes to the background, and from the Mac launcher once the host has exited.
// Which way: <DM_SAVE>.sync keeps the hash of the save last known to be the same here and in the cloud. Only the cloud's changed: it comes in; only this one changed: it goes out;
// both changed (played on both without syncing in between, or the first sync of a device that has a save of its own): the newer one wins and the other is kept.
// Every save this replaces, here or in the cloud, is kept first in <DM_SAVE>-backups/<date> <side>; the newest 10 of each side stay.
#pragma once
#include <stdint.h>

#define SS_HASH 32   // hex digits of a save's hash: the first 16 bytes of a SHA-256

typedef struct {
    char hash[SS_HASH + 1];   // over the paths, the guest names and the contents, not the dates: the same save has the same hash on every device
    int64_t saved;            // the newest file's modification time (seconds since 1970): which of two changed saves is newer
    char title[24], device[64];
    int files;                // regular files; 0 = no save
} ss_meta;

// ---- the parts (dm/host/tests/t_save_sync.c calls them) ----
// The committed save as it is on disk (save, and save + "_device"). 0 on success; m->files == 0: no save yet
int ss_scan(const char *save, ss_meta *m);
// Packs the save into one file. 0 on success; m receives what was packed
int ss_pack(const char *save, const char *title, const char *out, ss_meta *m);
// Reads a packed save and checks all of it (the header, every entry, the hash, the end marker); with `into`, unpacks it there as into/save and into/save_device.
// 0 on success; a damaged or cut-off file, or a path that leaves the save, fails
int ss_read(const char *bundle, const char *into, ss_meta *m);
typedef enum { SS_KEEP, SS_PULL, SS_PUSH } ss_way;
// Which way, from the three hashes ("" for none: no save here, no sync yet, nothing in the cloud) and, when both sides changed, their times. *both = both changed
ss_way ss_decide(const char *local, const char *base, const char *remote, int64_t local_saved, int64_t remote_saved, int *both);

// ---- the host ----
void save_sync_mount(const char *save, const char *title);   // the game's first save mount (hle_fs.c): brings the cloud's save in when it is the one to keep, starts the sender
void save_sync_committed(void);           // a Commit went through (hle_fs.c, under its locks: it only wakes the sender)
int save_sync_flush(int timeout_ms);      // sends now what was committed and not sent (the iPhone app going to the background): 1 = in step with the cloud, 0 = not (yet)
int save_sync_after_exit(const char *save, const char *title);   // the Mac launcher once the host has exited (a crash included): 1 = in step, 0 = not

// ---- what the platform provides (weak defaults in save_sync.c: plain file copies, which the Mac's iCloud Drive takes) ----
// Copies the cloud's file to a local one; the iPhone app goes through NSFileCoordinator, which downloads a file iCloud has only listed (SaveSync.m).
// 1 = copied, 0 = there is none, -1 = failed
int front_save_sync_fetch(const char *cloud, const char *local);
// Puts a local file at the cloud path, replacing the file there as a whole. 1 = done
int front_save_sync_store(const char *local, const char *cloud);
// Clones the committed save folder `dir` to `to` while no Commit can swap it (hle_fs.c takes its lock; the default, with no host running, does not need one). 0 on success
int dm_fs_clone_committed(const char *dir, const char *to);
