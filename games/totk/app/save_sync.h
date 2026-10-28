/* Save sync between devices: the account's save root on this device against the same folder on a sync server, through
   a small backend (WebDAV in app/webdav_sync.m; plain folders in tests/test_save_sync.c). The manifest of the last sync
   decides who changed what; saves are content, not devices, so both platforms' own account identities stay untouched.
   Never deletes a local file: a deletion made here propagates to the server, one made on another device is answered by
   uploading the local file again (an emptied server must not wipe a player's saves). When both sides changed the same
   file, the local one stays the live save and the other copy is kept in the backup folder (sync-conflict-<UTC>), next
   to a snapshot of the whole save root taken before the first local change (before-sync-<UTC>). A save root is a few
   files of a few MB (slot_00..05 with progress.sav and caption.sav), so listing the remote tree means downloading it. */
#pragma once
#include <stddef.h>

#define MNG_SYNC_STATE_NAME "sync-manifest.txt"   /* "<sha256> <path>" per line, under the remote root and beside the profile */

typedef struct mng_sync_backend {
    void *ctx;
    /* All calls return 0 on success and nonzero when the operation failed. Paths are relative to the remote root,
       '/'-separated, without a leading '/'. list returns every file of the tree (no folders) with its size; the arrays
       it makes are released with release. */
    int (*list)(void *ctx, char ***files, long long **sizes, int *count);
    int (*fetch)(void *ctx, const char *remote, const char *to_path);    /* copy a remote file to a local path */
    int (*store)(void *ctx, const char *remote, const char *from_path);  /* put a local file at the remote path */
    int (*remove)(void *ctx, const char *remote);                        /* delete a remote file */
    void (*release)(void *ctx, char **files, long long *sizes, int count);
} mng_sync_backend;

/* Run one sync. local_root is the save root on this device (made when missing), local_state holds the manifest of the
   last sync, staging receives the downloaded remote files (emptied here first), backups receives the snapshots. log, when
   given, is called with one line per event. Returns 0 when the sync ran and the counts were filled in; nonzero when it
   could not, with the local saves and the manifests unchanged (a partly applied run says so in the log). */
int mng_save_sync(const char *local_root, const char *local_state, const char *staging, const char *backups,
                  const mng_sync_backend *backend,
                  void (*log)(void *ud, const char *line), void *log_ud,
                  int *downloaded, int *uploaded, int *deleted_remote, int *conflicts);
