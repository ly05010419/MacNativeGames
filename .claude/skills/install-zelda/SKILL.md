---
name: install-zelda
description: Build and install The Legend of Zelda Tears of the Kingdom or Breath of the Wild from the player's own game files with build_app.py, for Mac, iPhone or iPad, and fix what stops the build. Use when a player asks to install, build, package or set up the game, or pastes a build error.
---

# Install Tears of the Kingdom / Breath of the Wild

You are helping a player turn **their own** game dump into an app. Done right, this is one build of 20–40 minutes, mostly waiting. Follow the steps in order. `docs/install.md` has the same steps for the player to read; you do not need it unless a step below points there.

## Rules

- **Only the player's own files.** Never search the internet for, download, or ask anyone for game files, `prod.keys`, title keys or firmware. If something is missing, tell the player what to dump from their own Switch and stop.
- **Never print, copy or paste key values.** Pass the path of `prod.keys` with `-k`; do not `cat` it.
- **Never weaken a check.** Do not edit `games/totk/assets/expected-*.json`, `build_app.py` or the Makefile to get past an error. A hash error means another version, an incomplete dump, or a fix that is already on a newer `main`. The error messages name the fix: read them.
- The app the build makes contains the game. It is for this player only; do not upload it or offer to share it.
- Speak the player's language. Find what you can yourself; ask only for what you cannot find (where the files are, which device).
- Commands that need `sudo`, a password or a click in a macOS dialog: ask the player to run them (in Claude Code, typing `! <command>` runs it in this session).

## 1. Check the Mac (about 2 minutes)

From the repository folder:

```sh
sw_vers -productVersion; uname -m
xcode-select -p; xcrun --show-sdk-path
ls -d /opt/homebrew/opt/opus /opt/homebrew/opt/libvpx; command -v python3.14
git log -1 --oneline; git status -sb | head -1
df -h .
```

| Needed | If not |
|---|---|
| `arm64` and macOS **26** or later (Metal 4) | Stop. macOS 15 is not supported here; a community fork has a Metal 3 backend: <https://github.com/1918662410-ship-it/MacNativeGames> (not tested by this repository). |
| Xcode command line tools, with a working SDK | `xcode-select --install` (the player clicks the dialog). |
| Homebrew in `/opt/homebrew` (the build looks only there) | Install from <https://brew.sh> (the player runs it; it asks for the password). |
| `brew install opus libvpx python@3.14` | Run it. Instead of Homebrew Python, `uv run --python 3.14 build_app.py …` works too. |
| The repository is current | `git pull` (or download a fresh ZIP). Many old build errors are fixed on `main`. |

Recommended when the full Xcode app is installed: `xcodebuild -downloadComponent MetalToolchain`. The build then compiles the startup shaders into the app, so it can be moved later and rebuilds are faster. Without it the build still works.

iPhone / iPad builds also need: full **Xcode 26+** with the iOS platform, `brew install xcodegen cmake ninja`, the player's Apple ID in Xcode › Settings › Accounts (a free one works), and the device on iOS / iPadOS 26, iPhone 14 or newer, with Developer Mode on.

## 2. Find and identify the player's files

Ask where they are, or look for names only (never open `prod.keys`):

```sh
find ~ /Volumes -maxdepth 5 \( -iname '*.xci' -o -iname '*.nsp' -o -iname '*.nsz' -o -iname 'prod.keys' \) 2>/dev/null
```

The title ID is usually in the file name in brackets. The build decides by the file's content, not its name, and says when a file is on the wrong option.

| Title ID | What | Goes to |
|---|---|---|
| `0100F2C0115B6000` | Tears of the Kingdom, base game | the first argument |
| `0100F2C0115B6800` | Tears of the Kingdom, update | `--update` |
| `01007EF00011E000` | Breath of the Wild, base game (or the 1.6.0 card image with update and DLC inside) | the first argument |
| `01007EF00011E800` | Breath of the Wild, update | `--update` |
| `01007EF00011F001`, `01007EF00011F002` | Breath of the Wild DLC packs (each optional) | `--dlc`, once per pack |

What is supported:
- **Tears of the Kingdom:** base **1.0.0** as an uncompressed XCI or NSP, plus optionally one update **1.1.2, 1.2.1 or 1.4.3** as NSP or NSZ (no intermediate updates needed). Prefer **1.4.3**: it is the most tested and the only version tested on iPhone / iPad. A card image that also holds its update is fine; the version still comes from `--update`.
- **Breath of the Wild:** the **1.6.0 card image** (base + update + DLC in one XCI), or a base NSP with the **1.6.0 or 1.9.0** update NSP and optional DLC NSPs.
- A compressed base game (NSZ / XCZ) is not accepted: decompress it with the player's own `nsz -D` first.

Other things the build needs:
- **`prod.keys`** from the same console (Lockpick_RCM). Keys dumped on an old firmware can be too old for a newer update.
- **System fonts** when the base is an **NSP**, and always for **Breath of the Wild**: `--firmware <folder of .nca files>` (the player's own firmware dump, unzipped), or `--shared-fonts <folder>` with the six `.bfttf` files an earlier Tears of the Kingdom build extracted (`TOTK_build/totk/extracted/firmware/shared-fonts`). A Tears of the Kingdom XCI carries them itself.
- **Disk:** the work folder goes next to the game file by default (`TOTK_build`, or `BotW_build`). Free space needed: about 25 GB for a fresh Mac build, 40 GB with an update; for iPhone / iPad 45 GB, 60 GB with an update. Keep the work folder and the app on the **same APFS volume** (the 17 GB of game data is cloned, not copied). If the game files are on an exFAT / FAT disk or a full one, add `--work <folder on an APFS disk>` and put `-o` on that disk too.

## 3. Pick the command

Run from the repository folder. Quote every path (file names often contain spaces and brackets). Choose the final place of the app now with `-o` into an existing folder: moving a Mac app later makes its first start compile the shaders again (unless the Metal toolchain was installed).

```sh
# Tears of the Kingdom 1.4.3, Mac, from an XCI
python3.14 build_app.py "<base 1.0.0.xci>" -k "<prod.keys>" --update "<1.4.3 update.nsp>" -o "/Applications/Tears of the Kingdom.app"

# from an NSP base: add the fonts
python3.14 build_app.py "<base 1.0.0.nsp>" -k "<prod.keys>" --update "<1.4.3 update.nsp>" --firmware "<firmware folder>" -o "/Applications/Tears of the Kingdom.app"

# Breath of the Wild 1.6.0 card image
python3.14 build_app.py "<botw 1.6.0.xci>" -k "<prod.keys>" --shared-fonts "<…/TOTK_build/totk/extracted/firmware/shared-fonts>" -o "/Applications/Breath of the Wild.app"

# Breath of the Wild 1.9.0 from NSPs
python3.14 build_app.py "<botw base.nsp>" -k "<prod.keys>" --update "<1.9.0 update.nsp>" --dlc "<dlc 1.nsp>" --dlc "<dlc 2.nsp>" --firmware "<firmware folder>" -o "/Applications/Breath of the Wild.app"

# iPhone / iPad: the same command plus these, and an .ipa output
  --platform ios --ios-game-data documents -o "<folder>/Tears of the Kingdom.ipa"
```

If `/Applications` is on another volume than the work folder, use a folder next to the work folder instead. Leaving out `--update` builds Tears of the Kingdom 1.0.0. Mods: `docs/mods.md`; Vision Pro: `docs/install.md` ("Apple Vision Pro"). Other games (Smash, Odyssey, …) are not built by `build_app.py`: see `games/<name>/README.md`.

## 4. Run it to the end

- A fresh build takes **20–40 minutes** on an M-series Mac (longer on 8 GB Macs): rewriting the code (~3 min), extracting and verifying 17 GB of data and merging the update (several minutes), preparing the shaders (~3 min), compiling them for the GPU (6–12 min) and macOS checking the new app once (2–3 min).
- Start it in the background or with a long timeout, and keep a copy of the output outside the repository, for example `… 2>&1 | tee ~/Library/Logs/MacNativeGames-build.log`. Read the log every few minutes. Lines look like `[12:03:41] 2/4 data: …`; `... still running (3 min)` is normal. Do **not** stop it for being quiet.
- If it was interrupted anyway, run **exactly the same command** again: the work folder keeps every finished step.
- Never run two builds on the same work folder at once, and quit the game before building.
- It worked when the last lines say `done in N min: <path of the app>`. Before that the build already started the app once with `--check` (a start check, not gameplay).

## 5. First start

**Mac:** `open "<the app>"`. Tell the player what to expect, so nothing looks like a hang:
- The start screen verifies the game data (about 20 s) and then prepares the shaders and pipelines with a progress bar. The first start takes 1–2 minutes (Tears of the Kingdom 1.4.3: 60–80 s on an M-series Mac); later starts about 12 s.
- An app moved or copied to another volume afterwards is checked by macOS again: no window for a few minutes. Wait.
- If macOS asks to allow access to files on a removable volume, the player clicks Allow.
- Short stutters the first time in a new place are expected.
- Key 1 opens the settings panel. Controls and settings: `docs/controls.md`.

**iPhone / iPad:** connect the device by cable, then:

```sh
python3.14 games/totk/ios/tools/install_device.py "<the .ipa>" --device "<device name as in Finder>" --game-data "<work folder>/totk/extracted/<version>/romfs/romfs.bin"
```

`<version>` is the folder that exists there (`v143` for Tears of the Kingdom 1.4.3; `ls "<work folder>/totk/extracted"`). The first install copies about 17 GB. A free Apple ID signs for 7 days: run the same command again to renew; saves are kept. On the device, trust the developer in Settings › General › VPN & Device Management if asked. The first start imports the shaders (about 3 minutes on an iPhone 14).

## 6. When the build stops

Read the `ERROR:` line and the log file it names (`<work folder>/logs/…`), fix the cause, then run the same command again.

| The error says | Cause and fix |
|---|---|
| `needs macOS 26 or later` | Not supported here; see step 1 (the fork). |
| `clang++ finds no C++ standard library headers` | Broken developer tools. With Xcode: `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`; without: `sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install`. |
| `Homebrew packages missing`, `Python 3.14 not found`, `git not found` | Install what it names. |
| `NSP files do not include system shared fonts`, `needs --firmware or --shared-fonts` | Add `--firmware` or `--shared-fonts` (step 2). |
| `SHA-256 … is not the expected …`, `ExeFS … is missing; not the supported version` | Another version or an incomplete dump. `git pull` first; check that the base is 1.0.0 (Tears of the Kingdom) and the update a supported version; otherwise the player dumps the game again. Never edit `expected-*.json`. |
| `unsupported update module hashes` | That update version is not supported; use one from step 2. |
| `the base game must be an uncompressed XCI or NSP` | Decompress the NSZ / XCZ base (`nsz -D`). |
| `is … add-on content (DLC), not the game`, `pass it as …`, `use the files of one game` | A file is on the wrong option; the message names the right one. |
| `the game file holds [...]: expected Tears of the Kingdom … or Breath of the Wild` | Another game: build it with its own `games/<name>/README.md`. |
| `needs about N GB free` | Free space, or `--work` (and `-o`) on an APFS volume with room. |
| `BKTR verification failed`, an update merge or key error | `git pull` first. If it persists, the keys may be older than the update: the player dumps `prod.keys` again from a console on current firmware. |
| `make failed` / `xcrun failed` (`… .log`) | Read the named log; usually incomplete developer tools or Homebrew outside `/opt/homebrew`. |
| `the app does not start: …` | Read the message; rerun once; otherwise report it (below). |
| iOS signing or install errors | Apple ID in Xcode › Settings › Accounts, device unlocked and trusted, Developer Mode on; run the install again. |

If it is still stuck, help the player open an issue at <https://github.com/ly05010419/MacNativeGames/issues> with: the command (paths shortened), Mac model, macOS version, `git log -1 --oneline`, the `ERROR:` line and the end of the log it names. Never attach keys, game files, firmware or the app.

## 7. Later

- After `git pull`, update the app without the game files: `python3.14 update_runtime.py "<the app>"` (about a minute). If it says the startup shaders no longer fit, add `--seed`.
- Another game version or a mod: run `build_app.py` again with the same `-o`; it replaces the app and keeps the saves (it backs them up before the first start on a new version).
- Saves: `~/Library/Application Support/MacNativeGames/TOTK/profile` (`BotW` for Breath of the Wild). The game log is off by default: `defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES` (`org.macnativegames.botw`), then `~/Library/Logs/MacNativeGames/totk.log` from the next start.
