# Save sync over WebDAV (macOS, iPhone, iPad)

Saves follow the player between their Mac and their iPhone through any WebDAV
server: 坚果云, Nextcloud, a self-hosted Apache or nginx, or the same Mac running
`games/totk/scripts/mini_webdav.py` on the home network. No iCloud, no entitlements,
nothing else to sign up for. The feature is off until a server is configured.

## Why WebDAV

A save root is small: `slot_00`..`slot_05` with `progress.sav` and `caption.sav`
per populated slot, a few files of a few MB each (the migration doc,
[save-migration.md](save-migration.md), describes the layout). It moves over any
connection in seconds. iCloud Drive would need an iCloud container entitlement on
the iPhone, which a free Apple ID cannot sign — WebDAV needs none, so the same
feature works everywhere the game runs.

## What is synced

The account's save root only — `profile/save/<account>/` on the Mac
(`~/Library/Application Support/MacNativeGames/TOTK/`), `Documents/save/` on the
iPhone. Shader caches, the game slot and the RomFS image are not save data and
stay local. Each device keeps its own account identity; the saves move as
content, and the game loads them whatever device wrote them. Start the game once
on a device before its first sync, so the account identity exists.

## Turning it on

Three values, the same keys everywhere. The easiest way on the Mac is the game's
own settings panel: **press 1 in the game**, and fill in the WebDAV address,
user and password at the bottom of the panel — the "Save sync (WebDAV)" row
above them switches the feature off and on (it stays off until an address is
filled in). What is typed there is stored the same way as the `defaults write`
below, and reaches the saves at the next start of the game. Or, from the
terminal:

```sh
defaults write org.macnativegames.totk TOTK_WEBDAV_URL https://dav.jianguoyun.com/dav
defaults write org.macnativegames.totk TOTK_WEBDAV_USER you@example.com
defaults write org.macnativegames.totk TOTK_WEBDAV_PASSWORD <app password>
```

(`org.macnativegames.botw` for Breath of the Wild.) On the iPhone they are the
**Settings › TOTK › Save sync (WebDAV)** fields. Environment variables of the
same names win — including `TOTK_WEBDAV_ENABLED=0`, which switches the sync off
wherever it is set. 坚果云 and Nextcloud take an app-specific password — use
that, not the account password.

The saves land in `MacNativeGames/<game>/saves/` on the server; the folder is
made on the first sync.

## How it merges

The sync runs when a game starts, and on the iPhone also when the app goes to
the background or ends (a save being written at that moment postpones it to the
next start). It compares three versions of every file: this device's, the
server's, and the manifest of the last sync (`sync-manifest.txt`, kept on both
sides):

- changed on one side only → that copy wins and travels;
- new on either side → copied to the other;
- deleted here while the server is unchanged → the deletion travels;
- deleted on the server while this device is unchanged → the local file is
  uploaded again, so an emptied server cannot wipe a player's saves;
- changed on both sides → **this device's file stays the live save**, the
  server's copy is kept under `profile/sync-backups/sync-conflict-<UTC>/`, and
  the two sides agree again. Before its first download of a run, the whole save
  root is copied to `sync-backups/before-sync-<UTC>/`.

Nothing deletes a local file. Restoring a backup is the same as for imports
([save-migration.md](save-migration.md)): quit the game, move the current
profile aside, move the backup back.

## What to expect

- The Mac uploads what a session wrote at the **next** start (the launcher
  starts the game and is gone); the iPhone uploads when it leaves the game. In
  practice: play on the iPhone, then start the game on the Mac once to receive.
- The runs log to the debug log (`[save-sync]` lines), off by default.
- Two devices syncing at the same moment are not coordinated against each
  other; the merge and the backups make the outcome safe (worst case, a
  conflict copy), but it is worth letting a sync finish before starting the
  other device.

## Trying it without an account

`scripts/mini_webdav.py` is a small WebDAV server for the home network:

```sh
python3 games/totk/scripts/mini_webdav.py --root ~/dav --user me --password secret &
defaults write org.macnativegames.totk TOTK_WEBDAV_URL http://<this-mac-ip>:8442
```

The engine is also covered by `tests/test_save_sync.c` (`make check`), and one
real round trip can be run without the game:

```sh
make -C games/totk probe-save-sync PROFILE=/tmp/save-sync-profile \
    TOTK_WEBDAV_URL=http://127.0.0.1:8442 TOTK_WEBDAV_USER=me TOTK_WEBDAV_PASSWORD=secret
```
