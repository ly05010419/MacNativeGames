# Multiple users, one save area each

As on a console, the game plays as one **user** at a time, and every user has their own save area. The users are
managed in the settings panel (key 1, the **User** tab) on the Mac.

## What a user is

- A user is a 128-bit id and a name. The game's save area is the folder named after that id:
  `<profile>/save/<32 hex digits>` (the profile is `~/Library/Application Support/MacNativeGames/TOTK/profile` for
  Tears of the Kingdom). Another user's saves are in another folder, so switching users switches saves.
- The name is what the game shows in its save screens (it asks the account system for it, as on a console).
- At most **8 users**, as many as a console holds. The first one is named "Local".

## Choosing a user

As on a console, the user is chosen **before the game starts** — the "preselected user" the system opens for the
game. Here that choice lives in the settings panel:

- **User (from the next launch)** lists the users. Picking one takes effect the **next time the game starts**; the
  game that is running keeps the user it opened with (its save area is already mounted, and switching mid-game
  would mix two players' progress).
- **User this run** shows the user the running game plays as (greyed out; it cannot change mid-run).
- **Add User…** (bottom right) names a new user in the game's text bar — the panel closes while the bar is up and
  comes back when the name is confirmed (Return) or cancelled (Escape). At most 10 characters, as on a console;
  the name is trimmed and control characters are dropped.

A user is never deleted by the game; their save folder stays until you remove it yourself (quit the game first).

## Where the users live

- The store is `<profile>/users.txt`, one user per line: an optional `*` first marks the user the next launch
  opens with, then 32 hex digits of id, a tab, and the name. It is written whole (a half-written store never
  survives) and is recovered line by line when a line does not parse.
- A profile from before the users (which only had the single identity file `account.uid`) keeps its identity as
  the user "Local"; `account.uid` stays for older builds. A fresh profile does not write it.

## Notes

- The feature is the runtime's (runtime/account.c, runtime/users.h); the settings panel is Mac-only, so on
  iPhone / iPad there is no way to switch users yet — the store itself works there (one user, "Local").
- Save sync (WebDAV, [save-sync-webdav](save-sync-webdav.md)) syncs the save areas it finds under
  `<profile>/save/`; with several users, every user's folder is synced the same way.
- Tested by `tests/test_users.c` (the store: migration, add / choose, restart, corrupt files) and the settings
  panel probe (`tests/test_settings_panel.py`, the User page, including adding a user through the text bar).
