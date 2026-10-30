# TOTK save migration on macOS

Import an unpacked save from Ryujinx, Yuzu or Eden. In the emulator, use **Open
Save Data Location** for TOTK and the intended user. Choose the folder containing
`slot_00` through `slot_05`, with `progress.sav` and `caption.sav` inside populated
slots. Close both the emulator and the native game before copying.

Preview the import from the repository root:

```sh
python3 games/totk/scripts/save_manager.py import "/path/to/emulator/save" --dry-run
```

Import it:

```sh
python3 games/totk/scripts/save_manager.py import "/path/to/emulator/save"
```

The default profile is
`~/Library/Application Support/MacNativeGames/TOTK/profile`. An existing profile's
`account.uid` is preserved. A fresh profile gets its own native identity; the
emulator's account ID is never substituted. All files in the selected save root,
including options, are copied without format conversion. The tool checks slot
structure and compares SHA-256 manifests before installing the copy. This does
not validate the game's internal save format or make newer saves compatible with
older game versions. Use the same game version as the source, or a version known
to load those saves.

An existing profile is kept beside it as
`profile-before-import-<UTC timestamp>-<unique suffix>`. The new profile is staged
in the same parent directory and installed with macOS's atomic directory exchange
only after a complete, verified backup is published. The live profile is never
temporarily removed; a failed exchange leaves it unchanged. The source is never moved or
modified. Import replaces the current user's entire save root, rather than
merging old and new slots. Other profile settings and other users' roots remain.

Export an emulator-compatible save root to a **new** folder:

```sh
python3 games/totk/scripts/save_manager.py export "/path/to/TOTK-save-export"
```

The exported folder contains the raw save files, without the native account or
profile wrapper. To return to an emulator, back up its existing save folder and
copy the export's contents into the folder opened by its save-location command.
Use the emulator's existing user; do not copy `account.uid` into it.

For an alternate native profile or game slot, pass `--profile PATH` and
`--slot-dir PATH`, or set `TOTK_PROFILE_DIR` and `TOTK_GAME_SLOT_DIR`. The slot
directory must match the app's slot directory. The tool holds the same exclusive
lock as the native runtime throughout import/export and refuses a running game.
The lock directory must be outside the profile and copied save folders, so a
profile replacement cannot replace the lock inode or copy it into a save.
Links and special files inside saves or profiles are refused.

To restore an import backup, quit the game, move the current profile aside, and
move the reported backup back to the original profile path. Keep both copies
until the restored save has loaded successfully.
