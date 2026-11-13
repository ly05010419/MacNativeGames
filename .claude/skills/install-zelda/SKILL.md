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
- **Folders (`docs/install.md` › Folders).** Keep the app and saves apart from the work folder: the work folder can be deleted and made again, the app and saves cannot. One work folder per game, outside the `MacNativeGames` checkout and outside the folder that holds the apps, on the app's APFS volume. Without `--work`, `build_app.py` and MNG Launcher both take the folder the app records, else `MacNativeGames-work/<game>` beside the apps folder (`build_app.default_work`): leave `--work` out unless the default disk lacks space.
- **Use MNG Launcher when the player wants a window, and always for updates.** Make it once: `python3 tools/builder_app/make_app.py` (it carries its own Python; it runs this checkout's code, so `git pull` is enough later). Open it with the files filled in: `open -a "MNG Launcher" "<base>" "<update>" "<prod.keys>"` (keys go to step 2, the rest to step 1); the player then presses **Build and Play**. Games already installed are listed at the top: **Update** needs no files and runs the fastest update that fits (`update_runtime.py --plan`: express, else fast; it asks for the files only when a full build is needed). Never start a full build for an installed game before that plan says so.
- **An existing install: check it first, and adapt it.** A player who built before 12 November 2026 (with `build_app.py` or the old `Build Game.command` window): walk them through `docs/install.md` › "Moving an older install to MNG Launcher". If the player already has the app, read `plutil -extract MNGWorkFolder raw "<the app>/Contents/Info.plist"` (and `MNGBuildPython`) before building or updating, and use that folder and that Python. If the key is missing (an app from before 31 October 2026), names a folder that is gone, or names one inside the apps folder (an update before 11 November 2026 without `--work` made one there), choose the work folder as above, `mv` the old one there when it holds the build, and run `update_runtime.py "<the app>" --work "<that folder>"` once: the app records it from then on. Tell the player what you moved.

## 1. Check the Mac (about 2 minutes)

From the repository folder:

```sh
sw_vers -productVersion; uname -m
xcode-select -p; xcrun --show-sdk-path
command -v python3.14
git log -1 --oneline; git status -sb | head -1
df -h .
```

| Needed | If not |
|---|---|
| `arm64` and macOS **26** or later (Metal 4) | Stop. macOS 15 is not supported here; a community fork has a Metal 3 backend: <https://github.com/1918662410-ship-it/MacNativeGames> (not tested by this repository). |
| Xcode command line tools, with a working SDK | `xcode-select --install` (the player clicks the dialog). |
| Homebrew | Not needed, for Mac or for iPhone / iPad. |
| Python 3.14 (`python3.14 --version`) | The player installs it from <https://www.python.org/downloads/macos/> (the .pkg puts `python3.14` in /usr/local/bin, where the build looks). `brew install python@3.14` or `uv run --python 3.14 build_app.py …` work too. opus, libvpx and dav1d are not installed: the build compiles them from pinned sources (once per Mac, under `~/Library/Caches/MacNativeGames/deps`). |
| The repository is current | `git pull` (or download a fresh ZIP). Many old build errors are fixed on `main`. |

Recommended when the full Xcode app is installed: `xcodebuild -downloadComponent MetalToolchain`. The build then compiles the startup shaders into the app, so it can be moved later and rebuilds are faster. Without it the build still works.

iPhone / iPad builds also need: full **Xcode 26+** with the iOS platform (XcodeGen, CMake and Ninja come with the build, pinned), the player's Apple ID in Xcode › Settings › Accounts (a free one works), and the device on iOS / iPadOS 26, iPhone 14 or newer, with Developer Mode on.

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
- **Tears of the Kingdom:** base **1.0.0** as an uncompressed XCI or NSP, plus optionally one update **1.2.1 or 1.4.3** as NSP or NSZ (no intermediate updates needed). Prefer **1.4.3**: it is the most tested and the only version tested on iPhone / iPad. A card image that also holds its update is fine; the version still comes from `--update`.
- **Breath of the Wild:** the **1.6.0 card image** (base + update + DLC in one XCI), or a base NSP with the **1.6.0 or 1.9.0** update NSP and optional DLC NSPs.
- A compressed base game (NSZ / XCZ) is not accepted: decompress it with the player's own `nsz -D` first.

Other things the build needs:
- **`prod.keys`** from the same console as the game files. A keys file older than the update cannot open it.
- **System fonts** when the base is an **NSP**, and always for **Breath of the Wild**: `--firmware <folder of .nca files>` (the player's own firmware dump, unzipped), or `--shared-fonts <folder>` with the six `.bfttf` files an earlier Tears of the Kingdom build extracted (`<work folder>/totk/extracted/firmware/shared-fonts` (by default `~/MacNativeGames-work/totk/…`)). A Tears of the Kingdom XCI carries them itself.
- **Disk:** the work folder is `MacNativeGames-work/<game>` beside the apps folder by default (or the one the app records). Free space needed: about 25 GB for a fresh Mac build, 40 GB with an update; for iPhone / iPad 45 GB, 60 GB with an update. Keep the work folder and the app on the **same APFS volume** (the 17 GB of game data is cloned, not copied). If the game files are on an exFAT / FAT disk or a full one, add `--work <folder on an APFS disk>` and put `-o` on that disk too.

## 3. Pick the command

**Use `tools/mng.py` (MNG Launcher's engine in a terminal), not `build_app.py` by hand.** It is what the Launcher runs: it tells the files apart, checks the Mac, puts the app and its work folder where the one folder rule says, takes the fastest update when the game is already installed, and runs as the Mac's one job, so the player's Launcher window shows its progress and can stop it. Any `python3` starts it (it then uses MNG Launcher's own Python, or Python 3.14).

**First, always: `python3 tools/mng.py migrate`.** It says whether this Mac is migrated to MNG Launcher and the folder rule; when it is not, run `python3 tools/mng.py migrate --apply` (it moves work folders on the same disk, records them with express updates, makes the Launcher), tell the player what it did, and only then go on. Open MNG Launcher only on a migrated Mac (`mng open` refuses before).

```sh
python3 tools/mng.py migrate [--apply]                        # first: is this Mac migrated? --apply finishes it
python3 tools/mng.py status                                   # the games on this Mac and the update each needs
python3 tools/mng.py check "<base>" "<update>" --keys "<prod.keys>"   # what the files are, what the Mac still needs
python3 tools/mng.py build "<base>" "<update>" --keys "<prod.keys>"   # build (or update) that game
python3 tools/mng.py update totk                              # an installed game, no files: express, else fast
python3 tools/mng.py open "<base>" "<prod.keys>"              # or let the player press the button in MNG Launcher
```

Add `--firmware`-type inputs as more files (a firmware or shared-fonts folder is recognised like in the window). `--json` prints what the window reads. The `build_app.py` commands below are what `mng` runs; use them directly only for what `mng` does not do (iPhone / iPad, `--mod`, `--vr`, other options).

Run from the repository folder. Quote every path (file names often contain spaces and brackets). Choose the final place of the app now with `-o` into an existing folder: moving a Mac app later makes its first start compile the shaders again (unless the Metal toolchain was installed).

```sh
# Tears of the Kingdom 1.4.3, Mac, from an XCI
python3.14 build_app.py "<base 1.0.0.xci>" -k "<prod.keys>" --update "<1.4.3 update.nsp>" -o "/Applications/Tears of the Kingdom.app"

# from an NSP base: add the fonts
python3.14 build_app.py "<base 1.0.0.nsp>" -k "<prod.keys>" --update "<1.4.3 update.nsp>" --firmware "<firmware folder>" -o "/Applications/Tears of the Kingdom.app"

# Breath of the Wild 1.6.0 card image
python3.14 build_app.py "<botw 1.6.0.xci>" -k "<prod.keys>" --shared-fonts "<…/MacNativeGames-work/totk/totk/extracted/firmware/shared-fonts>" -o "/Applications/Breath of the Wild.app"

# Breath of the Wild 1.9.0 from NSPs
python3.14 build_app.py "<botw base.nsp>" -k "<prod.keys>" --update "<1.9.0 update.nsp>" --dlc "<dlc 1.nsp>" --dlc "<dlc 2.nsp>" --firmware "<firmware folder>" -o "/Applications/Breath of the Wild.app"

# iPhone / iPad: the same command plus these, and an .ipa output
  --platform ios --ios-game-data documents -o "<folder>/Tears of the Kingdom.ipa"
```

If `/Applications` is on another volume than the work folder, use a folder next to the work folder instead. Leaving out `--update` builds Tears of the Kingdom 1.0.0. Mods: `docs/mods.md`; Vision Pro: `docs/install.md` ("Apple Vision Pro"). Other games (Smash, Odyssey, …) are not built by `build_app.py`: see `games/<name>/README.md`; Red Alert 2 has its own skill (`.claude/skills/install-ra2/SKILL.md`).

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

`<version>` is the folder that exists there (`v143` for Tears of the Kingdom 1.4.3; `ls "<work folder>/totk/extracted"`). The first install copies about 17 GB, so the device must be on the cable: over Wi-Fi the script stops (`--wifi` forces it; about 2 minutes a GiB, and it drops). A free Apple ID signs for 7 days: run the same command again to renew; saves are kept. On the device, trust the developer in Settings › General › VPN & Device Management if asked. The first start imports the shaders (about 3 minutes on an iPhone 14).

## 6. When the build stops

Read the `ERROR:` line and the log file it names (`<work folder>/logs/…`), fix the cause, then run the same command again.

| The error says | Cause and fix |
|---|---|
| `needs macOS 26 or later` | Not supported here; see step 1 (the fork). |
| `clang++ finds no C++ standard library headers` | Broken developer tools. With Xcode: `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`; without: `sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install`. |
| `Python 3.14 not found`, `git not found` | Install what it names. |
| `[deps] … failed` | tools/deps makes opus, libvpx, dav1d, XcodeGen, CMake… itself: read the log it names (`~/Library/Caches/MacNativeGames/deps/logs`); a failed download, run the build again. Never suggest Homebrew for these. |
| `NSP files do not include system shared fonts`, `needs --firmware or --shared-fonts` | Add `--firmware` or `--shared-fonts` (step 2). |
| `SHA-256 … is not the expected …`, `ExeFS … is missing; not the supported version` | Another version or an incomplete dump. `git pull` first; check that the base is 1.0.0 (Tears of the Kingdom) and the update a supported version; otherwise the player dumps the game again. Never edit `expected-*.json`. |
| `unsupported update module hashes` | That update version is not supported; use one from step 2. |
| `the base game must be an uncompressed XCI or NSP` | Decompress the NSZ / XCZ base (`nsz -D`). |
| `is … add-on content (DLC), not the game`, `pass it as …`, `use the files of one game` | A file is on the wrong option; the message names the right one. |
| `the game file holds [...]: expected Tears of the Kingdom … or Breath of the Wild` | Another game: build it with its own `games/<name>/README.md`. |
| `needs about N GB free` | Free space, or `--work` (and `-o`) on an APFS volume with room. |
| `the work folder … records, …, cannot be found, nor the folder it was in` | The app remembers a work folder on a disk that is not connected or a folder that was moved. Ask the player whether that disk can be connected; if yes, run the same command again; if not, choose a work folder by the Folders rule and add `--work "<it>"` (the app records it). A deleted work folder alone is made again by the update itself. |
| `BKTR verification failed`, an update merge or key error | `git pull` first. If it persists, the keys may be older than the update; tell the player so, and never explain how keys are obtained. |
| `make failed` / `xcrun failed` (`… .log`) | Read the named log; usually incomplete developer tools. |
| `the app does not start: …` | Read the message; rerun once; otherwise report it (below). |
| iOS signing or install errors | Apple ID in Xcode › Settings › Accounts, device unlocked and trusted, Developer Mode on; run the install again. |

If it is still stuck, help the player open an issue at <https://github.com/ly05010419/MacNativeGames/issues> with: the command (paths shortened), Mac model, macOS version, `git log -1 --oneline`, the `ERROR:` line and the end of the log it names. Never attach keys, game files, firmware or the app.

## 7. Later

- Updating the Mac app: `python3 tools/mng.py update <game>` (or **Update** in MNG Launcher) runs the fastest update that fits. Also: the settings panel (key 1) › **Update** shows the version and each newer commit; its **Update** button pulls the code, updates the app and opens it again. When it cannot (another branch, local changes, no access to the folder), it says why: then `git pull` and `python3.14 update_runtime.py "<the app>"`. `python3.14 update_runtime.py "<the app>" --plan` says which update fits (JSON: express, fast or full, and why); MNG Launcher's **Update** runs it for the player. Follow docs/install.md "Update the app" in its fixed order: the express update (`update_runtime.py`) first; the fast update (`--seed`) when it says the startup shaders no longer fit; the normal update (`build_app.py` with the same `-o` and the recorded `--work`) when it says a full build is needed. It uses the work folder the app records and prints it; check that line names the folder you expect.
- Another game version or a mod: run `build_app.py` again with the same `-o`; it replaces the app and keeps the saves (it backs them up before the first start on a new version).
- Saves: `~/Library/Application Support/MacNativeGames/TOTK/profile` (`BotW` for Breath of the Wild). The game log is off by default: `defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES` (`org.macnativegames.botw`), then `~/Library/Logs/MacNativeGames/totk.log` from the next start.
