/* The WebDAV backend of app/save_sync.h, plus the configuration the Mac launcher and the iPhone app share:
   TOTK_WEBDAV_URL / TOTK_WEBDAV_USER / TOTK_WEBDAV_PASSWORD in the environment win, then NSUserDefaults (the iPhone's
   Settings pane, `defaults write org.macnativegames.totk TOTK_WEBDAV_URL ...` on the Mac; Basic auth over HTTPS).
   Off unless a URL is configured. Works with the common servers (坚果云, Nextcloud, Apache, nginx): PROPFIND with
   Depth 1 only, GET, PUT, MKCOL, DELETE. */
#pragma once

typedef struct { const char *url, *user, *password; } mng_webdav_config;

/* Read the configuration; url is NULL (or empty) when the sync is off. */
mng_webdav_config mng_webdav_config_read(void);

/* One full sync of the account's save root (profile/save/<uid>) with <url>/MacNativeGames/<game_folder>/saves, through
   app/save_sync.c. Never raises and never blocks the game: everything is logged to stderr (the debug log; the iPhone's
   run has debug on while the sync goes), failures return with the saves as they were. Skips itself when the account
   identity is not there yet (the game makes it at its first start, as scripts/save_manager.py expects) or when a game
   holds the slot lock (caller decides for itself where that cannot happen; on the iPhone pass slot_dir NULL and call
   this only when runtime_fs_save_busy() is false). */
void mng_webdav_sync_profile(const char *game_folder, const char *profile, const char *slot_dir);
