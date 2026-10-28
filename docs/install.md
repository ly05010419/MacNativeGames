# Install

[← README](../README.md) · [中文](install.zh-CN.md)

This page only covers turning **your own** game files into an app on your Mac, iPhone or iPad. One build takes 20–40 minutes, most of it waiting. Controls and settings: [controls.md](controls.md). What has been tested: [test-records.md](test-records.md). AI assistants: the same steps as a checklist are in the [install skill](../.claude/skills/install-zelda/SKILL.md).

Contents: [What you need](#what-you-need) · [Build the app](#build-the-app) · [Breath of the Wild](#breath-of-the-wild) · [iPhone and iPad](#iphone-and-ipad) · [Apple Vision Pro](#apple-vision-pro) · [Update the app](#update-the-app) · [When the build stops](#when-the-build-stops) · [Where things are](#where-things-are) · [Known issues](#known-issues)

Ground rules: use only files from your own Switch and your own game. Nothing is downloaded for you, and nothing from the game is in this repository. Never edit `games/totk/assets/expected-*.json` or the scripts to get past a check: the error message says what is wrong. The app you build contains the game: it is for you only.

## What you need

**A Mac** with Apple silicon and **macOS 26** or later (the graphics use Metal 4). 16 GB of memory or more is recommended.
On macOS 15, use the community fork by [1918662410-ship-it](https://github.com/1918662410-ship-it/MacNativeGames), which adds a Metal 3 backend (not tested here).

**Your game files**, dumped from your own Switch. The title ID is usually in the file name in brackets; the build reads the file itself, never its name.

| Title ID | What | On the command line |
|---|---|---|
| `0100F2C0115B6000` | Tears of the Kingdom, base game **1.0.0**, XCI or NSP | first argument |
| `0100F2C0115B6800` | Tears of the Kingdom update **1.1.2, 1.2.1 or 1.4.3**, NSP or NSZ (optional) | `--update` |
| `01007EF00011E000` | Breath of the Wild base game, or the **1.6.0** card image (base + update + DLC in one XCI) | first argument |
| `01007EF00011E800` | Breath of the Wild update **1.6.0 or 1.9.0**, NSP or NSZ | `--update` |
| `01007EF00011F001`, `01007EF00011F002` | Breath of the Wild DLC packs, NSP (each optional) | `--dlc`, once per pack |

- For Tears of the Kingdom, **1.4.3** is recommended: it is the most tested and the only version tested on iPhone and iPad. No intermediate update is needed. Without `--update` you get 1.0.0.
- The base game must be uncompressed. Decompress an NSZ or XCZ base first (`nsz -D`).
- A card image that also holds its own update is fine; the version still comes from `--update`.

**`prod.keys`** from the same Switch (Lockpick_RCM). Keys dumped on an old firmware can be too old for a newer update.

**System fonts**, needed when the base game is an **NSP** and always for **Breath of the Wild**. Give one of:
- `--firmware <folder>`: your firmware dump as a folder of `.nca` files (unzip it first), or an installed firmware `registered` folder.
- `--shared-fonts <folder>`: the six `.bfttf` files an earlier Tears of the Kingdom build extracted, in `TOTK_build/totk/extracted/firmware/shared-fonts`.

A Tears of the Kingdom XCI carries the fonts itself.

**Free disk space** on the volume of the work folder:

| | Mac | iPhone / iPad |
|---|---|---|
| base game only | 25 GB | 45 GB |
| with an update | 40 GB | 60 GB |

The work folder is created next to the game file (`TOTK_build`, or `BotW_build` for Breath of the Wild). Keep it and the app on the **same APFS volume**: the 17 GB of game data is then cloned, not copied. If your game files are on an exFAT/FAT disk or a full one, pass `--work <folder>` and `-o <app>` on an APFS disk with room.

## Build the app

**1. Tools** (once). An internet connection is needed:

```sh
xcode-select --install
# Homebrew, if missing: see https://brew.sh (it must live in /opt/homebrew)
brew install opus libvpx python@3.14
```

Instead of Homebrew's Python you can use [uv](https://docs.astral.sh/uv/): `uv run --python 3.14 build_app.py …` in place of `python3.14 build_app.py …`.

Optional, if the full Xcode app is installed: `xcodebuild -downloadComponent MetalToolchain`. The build then compiles the startup shaders into the app, so the app can be moved and rebuilds are faster. Without it the build still works, and the shaders go into macOS's Metal cache for the app's location.

**2. This repository**: `git clone https://github.com/ly05010419/MacNativeGames.git` (or download the ZIP), then open Terminal in the `MacNativeGames` folder. If you already have it, run `git pull` first: many build errors are fixed on `main`.

**3. Build.** Put each path in quotes. Pick the app's final place now with `-o`: moving a Mac app later makes its first start compile the shaders again (unless the Metal toolchain was installed). Without `-o` the app is written next to the game file.

```sh
# Tears of the Kingdom 1.4.3 from an XCI
python3.14 build_app.py "<base 1.0.0.xci>" -k "<prod.keys>" --update "<1.4.3 update.nsp>" -o "/Applications/Tears of the Kingdom.app"

# Tears of the Kingdom 1.4.3 from an NSP: also give the fonts
python3.14 build_app.py "<base 1.0.0.nsp>" -k "<prod.keys>" --update "<1.4.3 update.nsp>" --firmware "<firmware folder>" -o "/Applications/Tears of the Kingdom.app"
```

If `/Applications` is on another volume than the work folder, choose a folder on the work folder's volume instead.

**4. Let it run to the end.** A fresh build takes **20–40 minutes** on an M-series Mac, longer on an 8 GB Mac:

| Step | Time |
|---|---|
| `1/4 code`: rewriting the game code for Apple silicon | about 3 min |
| `2/4 data`: extracting and verifying 17 GB, merging the update | several minutes |
| `3/4 shaders`: preparing the startup shaders | about 3 min |
| compiling them for your GPU | 6–12 min |
| `4/4 app`: macOS checks the new app once | 2–3 min |

- Long quiet stretches and lines like `... still running (3 min)` are normal. Do not stop it.
- If it was interrupted, run **exactly the same command** again. The work folder keeps every finished step.
- Quit the game before building, and never run two builds on the same work folder.
- It worked when the last lines say `done in N min: <path of the app>`.

**5. Open the app** from Finder.
- The start screen verifies the game data (about 20 s), then prepares the shaders and pipelines with a progress bar. The first start takes 1–2 minutes; later starts take about 12 s.
- An app moved to another volume afterwards is checked by macOS again: no window for a few minutes. Wait.
- If macOS asks for access to files on a removable volume, click Allow.
- New places can stutter briefly the first time.
- Key **1** opens the settings panel. Controls: [controls.md](controls.md).

## Breath of the Wild

The same script builds Breath of the Wild. It always needs fonts (see [What you need](#what-you-need)). The work folder is `BotW_build` and the app is `Breath of the Wild.app`. A fresh build takes about 16 minutes on an M4.

```sh
# 1.6.0 from the card image (base + update + DLC in one XCI)
python3.14 build_app.py "<botw 1.6.0.xci>" -k "<prod.keys>" --shared-fonts "<…/TOTK_build/totk/extracted/firmware/shared-fonts>" -o "/Applications/Breath of the Wild.app"

# 1.9.0 (or 1.6.0) from NSPs: base + update, each DLC pack optional
python3.14 build_app.py "<botw base.nsp>" -k "<prod.keys>" --update "<1.9.0 update.nsp>" --dlc "<dlc 1.nsp>" --dlc "<dlc 2.nsp>" --firmware "<firmware folder>" -o "/Applications/Breath of the Wild.app"
```

With a separate update the app holds only the DLC packs you give with `--dlc`. A card image brings its own DLC: leave out `--dlc` then.

## iPhone and iPad

**Needed:**
- iOS / iPadOS 26 and an iPhone 14 or newer.
- On the Mac: the full **Xcode 26** or later with the iOS platform, plus `brew install xcodegen cmake ninja`.
- Your Apple ID in Xcode › Settings › Accounts. A free one works.
- Developer Mode on the device.

**1. Build an IPA.** Use the Mac command above, add `--platform ios --ios-game-data documents`, and give `-o` an `.ipa` file:

```sh
python3.14 build_app.py "<base 1.0.0.xci>" -k "<prod.keys>" --update "<1.4.3 update.nsp>" --platform ios --ios-game-data documents -o "<folder>/Tears of the Kingdom.ipa"
```

The first iOS build also downloads and compiles device libraries. `--ios-game-data documents` leaves the 17 GB game image out of the IPA, so later installs take minutes.

**2. Install it** with the device connected by cable:

```sh
python3.14 games/totk/ios/tools/install_device.py "<folder>/Tears of the Kingdom.ipa" --device "<device name as in Finder>" --game-data "<work folder>/totk/extracted/<version>/romfs/romfs.bin"
```

- `<version>` is the folder that is there (`v143` for 1.4.3; `ls "<work folder>/totk/extracted"`). The first install copies the 17 GB image once.
- A free Apple ID signs for 7 days, a paid one for a year. Run the same command again to renew; your saves are kept.
- If the device asks, trust the developer in Settings › General › VPN & Device Management.

**3. First start:** about 15 s to check the game data, then about 3 minutes of shader import before the title (iPhone 14). Later starts take about 8 s.

On the phone the game renders at 960×540, upscaled to 1280×720, at up to 60 fps. Breath of the Wild 1.6.0 builds and installs the same way.

Two limits:
- A phone cannot translate shaders while you play: objects whose shader variant the app does not carry are skipped, so a few things can be missing in places no player has reported yet.
- On an iPhone 14, iOS can close the game when memory runs out in busy areas.

## Apple Vision Pro

Tears of the Kingdom only, windowed 16:9 play with a controller. Install the visionOS platform in Xcode › Settings › Components, pair the headset with Xcode and turn on Developer Mode, then:

```sh
python3.14 build_app.py "<base 1.0.0.xci>" -k "<prod.keys>" --platform visionos --ios-game-data documents -o "<folder>/TOTK-Vision.app"
python3.14 games/totk/ios/tools/install_device.py "<folder>/TOTK-Vision.app" --device "<Vision Pro name>" --game-data "<work folder>/totk/extracted/v0/romfs/romfs.bin"
```

With `--update`, use that version's folder (`v112`, `v121` or `v143`) instead of `v0`.

## Update the app

- **New version of this repository** (after `git pull`): `python3.14 update_runtime.py "<the app>"` updates it in about a minute, without the game files or keys. Quit the game first. If it says the startup shaders no longer fit, add `--seed`.
- **Another game version:** run `build_app.py` again with the new `--update` and the same `-o`. It replaces the app. Before the first start on the new version, the app backs up your saves next to them as `profile-before-<version>`; keep that backup if you may go back.
- **Mods:** `--mod "<mod folder>" --install-mod-romfs` builds the app with a mod's code patches and installs its data. See [mods.md](mods.md), [merged-mod-pack.md](merged-mod-pack.md) and [translation-mods.md](translation-mods.md).
- **One desktop entry** (optional): `python3.14 games/totk/tools/install_desktop_entry.py --app "<the app>"`.

## When the build stops

Read the `ERROR:` line and the log file it names (`<work folder>/logs/…`), fix the cause, and run the same command again.

| The error says | Fix |
|---|---|
| `needs macOS 26 or later` | Not supported here; see the macOS 15 fork under [What you need](#what-you-need). |
| `clang++ finds no C++ standard library headers` | With Xcode: `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`. Without: `sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install`. |
| `Homebrew packages missing`, `Python 3.14 not found`, `git not found` | Install what it names. |
| `NSP files do not include system shared fonts`, `needs --firmware or --shared-fonts` | Add `--firmware` or `--shared-fonts`. |
| `SHA-256 … is not the expected …`, `ExeFS … is missing; not the supported version` | `git pull` first. Then check that the base is 1.0.0 and the update a supported version; otherwise dump the game again. |
| `unsupported update module hashes` | That update version is not supported. |
| `the base game must be an uncompressed XCI or NSP` | Decompress the NSZ / XCZ base (`nsz -D`). |
| `… add-on content (DLC), not the game`, `pass it as …`, `use the files of one game` | A file is on the wrong option; the message names the right one. |
| `the game file holds [...]: expected Tears of the Kingdom … or Breath of the Wild` | Another game: build it with its own `games/<name>/README.md`. |
| `needs about N GB free` | Free space, or `--work` and `-o` on an APFS volume with room. |
| `BKTR verification failed`, an error merging the update | `git pull` first. If it stays, your keys may be older than the update: dump `prod.keys` again on current firmware. |
| `make failed`, `xcrun failed` | Read the named log; usually incomplete developer tools or Homebrew outside `/opt/homebrew`. |
| iOS signing or install errors | Apple ID in Xcode › Settings › Accounts, device unlocked and trusted, Developer Mode on; run the install again. |

Still stuck? Open an [issue](https://github.com/ly05010419/MacNativeGames/issues) with the command (paths shortened), Mac model, macOS version, `git log -1 --oneline`, the `ERROR:` line and the end of the log it names. Never attach keys, game files, firmware or the app.

## Where things are

Tears of the Kingdom; for Breath of the Wild replace `TOTK` with `BotW` and `totk` with `botw`.

- Saves: `~/Library/Application Support/MacNativeGames/TOTK/profile`
- Game data (only for an app built with `--mac-game-data library` or `--mac-game-data-folder`): `~/Library/Application Support/MacNativeGames/TOTK/GameData/<version>` (`BotW` for Breath of the Wild) holds the 16 GB RomFS image `romfs.bin`, and `romfs.sha256`, which the build writes once the copy is complete. By default the image is inside the app. With `--mac-game-data library` the build puts it in this folder once (an APFS clone when the work folder is on the same volume, otherwise a copy), and later builds leave it alone while it matches the version's pin. The app is then a few hundred MB, so a new build installs or copies to another Mac quickly. To keep it somewhere else, for example on another disk, add `--mac-game-data-folder <folder>` to the build instead (that alone is enough; the image goes in `<folder>/<version>`, so one folder can hold both games). The app remembers the folder, and when it holds no complete image (another Mac, or the disk is not connected) the launcher looks in `GameData` instead. When neither has it, the app asks where the game data is: connect the disk and choose Try Again, or choose the folder that holds `romfs.bin` (or its `<version>` folder). It reads the image where it is, without moving or copying it, and remembers the folder for the next starts (`defaults delete org.macnativegames.botw MNGGameDataFolder.botw190` forgets it). The game still checks the image against the pin at its first start. `make_mod_manifest.py --app` and `make_button_icons.py --app` find the image the same way (`--romfs-data <folder>` names it).
- Shader cache: `~/Library/Application Support/MacNativeGames/TOTK/shader-cache`
- Shaders the shared list lacks: `~/Library/Application Support/MacNativeGames/TOTK/shader-list-contribution.txt` (the game asks before it starts whether to send them; see the [shared shader list](shader-list.md))
- Mods: `~/Library/Application Support/MacNativeGames/TOTK/mods/<v0|v112|v121|v143>`; see [mods.md](mods.md)
- Log: off by default. `defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES` turns it on from the next start, written to `~/Library/Logs/MacNativeGames/totk.log`. On an iPhone: Settings › TOTK › Debug log, written to the app's `Documents/log.txt`.

## Known issues

**Requests beyond this page are welcome.** The versions and games listed are the ones we could test. If you need another version or another game, the AI assistant should do its best to make it work.

- Tears of the Kingdom: base 1.0.0, optionally with the 1.1.2, 1.2.1 or 1.4.3 update. Breath of the Wild: 1.6.0 and 1.9.0. Other versions, compressed base games and personalized tickets are not supported.
- No online features, no amiibo.
- New shaders can stutter the first time; the cache and the [shared shader list](shader-list.md) reduce it.
