# Install

[← README](../README.md) · [中文](install.zh-CN.md)

This page only covers turning **your own** game files into an app on your Mac, iPhone or iPad. One build takes 20–40 minutes, most of it waiting. Controls and settings: [controls.md](controls.md). What has been tested: [test-records.md](test-records.md). AI assistants: the same steps as a checklist are in the [install skill](../.claude/skills/install-zelda/SKILL.md).

**Ocarina of Time** (Switch 2) has its own page: [games/oot/docs/install.md](../games/oot/docs/install.md).

Contents: [What you need](#what-you-need) · [Build the app](#build-the-app) · [Breath of the Wild](#breath-of-the-wild) · [iPhone and iPad](#iphone-and-ipad) · [Apple Vision Pro](#apple-vision-pro) · [Update the app](#update-the-app) · [Folders](#folders) · [When the build stops](#when-the-build-stops) · [Where things are](#where-things-are) · [Known issues](#known-issues)

Ground rules: use only files from your own Switch and your own game. Nothing is downloaded for you, and nothing from the game is in this repository. Never edit `games/totk/assets/expected-*.json` or the scripts to get past a check: the error message says what is wrong. The app you build contains the game: it is for you only.

## What you need

**A Mac** with Apple silicon and **macOS 26** or later (the graphics use Metal 4). 16 GB of memory or more is recommended.
On macOS 15, use the community fork by [1918662410-ship-it](https://github.com/1918662410-ship-it/MacNativeGames), which adds a Metal 3 backend (not tested here).

**Your game files**, dumped from your own Switch. The title ID is usually in the file name in brackets; the build reads the file itself, never its name.

| Title ID | What | On the command line |
|---|---|---|
| `0100F2C0115B6000` | Tears of the Kingdom, base game **1.0.0**, XCI or NSP | first argument |
| `0100F2C0115B6800` | Tears of the Kingdom update **1.2.1 or 1.4.3**, NSP or NSZ (optional) | `--update` |
| `01007EF00011E000` | Breath of the Wild base game, or the **1.6.0** card image (base + update + DLC in one XCI) | first argument |
| `01007EF00011E800` | Breath of the Wild update **1.6.0 or 1.9.0**, NSP or NSZ | `--update` |
| `01007EF00011F001`, `01007EF00011F002` | Breath of the Wild DLC packs, NSP (each optional) | `--dlc`, once per pack |

- For Tears of the Kingdom, **1.4.3** is recommended: it is the most tested and the only version tested on iPhone and iPad. No intermediate update is needed. Without `--update` you get 1.0.0.
- The base game must be uncompressed. Decompress an NSZ or XCZ base first (`nsz -D`).
- A card image that also holds its own update is fine; the version still comes from `--update`.

**`prod.keys`** from the same Switch (Lockpick_RCM). Keys dumped on an old firmware can be too old for a newer update.

**System fonts**, needed when the base game is an **NSP** and always for **Breath of the Wild**. Give one of:
- `--firmware <folder>`: your firmware dump as a folder of `.nca` files (unzip it first), or an installed firmware `registered` folder.
- `--shared-fonts <folder>`: the six `.bfttf` files an earlier Tears of the Kingdom build extracted, in `<work folder>/totk/extracted/firmware/shared-fonts` (by default `~/MacNativeGames-work/totk/…`).

A Tears of the Kingdom XCI carries the fonts itself.

**Free disk space** on the volume of the work folder:

| | Mac | iPhone / iPad |
|---|---|---|
| base game only | 25 GB | 45 GB |
| with an update | 40 GB | 60 GB |

Without `--work`, the work folder is the one the app at `-o` records, else `MacNativeGames-work/<game>` beside the folder that holds the app (for `~/Applications`: `~/MacNativeGames-work/totk`; for `/Applications`: `~/MacNativeGames-work/totk` too). MNG Launcher uses the same rule. Keep it and the app on the **same APFS volume**: the 17 GB of game data is then cloned, not copied. If your game files are on an exFAT/FAT disk or a full one, pass `--work <folder>` and `-o <app>` on an APFS disk with room.

## Build with a window

**New: MNG Launcher, the drag-and-drop builder.** `python3 tools/builder_app/make_app.py` makes `~/Applications/MNG Launcher.app` (it carries its own Python): step 1 drop the game, step 2 choose your `prod.keys`, step 3 **Build and Play**, with the progress shown the whole way. Games already on this Mac are listed at the top with an **Update** button: it needs no game files and runs the fastest update that fits (express, else fast; a full build only when it must). Started as `open -a "MNG Launcher" "<game file>" "<update>" "<prod.keys>"` (what an AI helping you does), it opens with steps 1 and 2 filled in. Mac apps of every game for now; details in [builder-app.md](builder-app.md). **Build and update with MNG Launcher.**

MNG Launcher also has the pages of the older window, which is gone (no more **Build Game.command** or `python3.14 -m tools.builder`): **Build**, **Settings** (how Tears of the Kingdom 1.4.3 and Breath of the Wild 1.9.0 start, and **Play**), **Mods** (your own mods, checked against your app and applied with the repository's installer; the builder ships none), **Controls** (the controller layout in `controls.txt`, the button icons and a key test), **Storage** (the work folder and the shader cache) and **Saves** (back up and restore). Each page tells you what it does; details in [builder-app.md](builder-app.md). How the pages' models are made, for anyone changing them (people or AI assistants): [builder.md](builder.md).

## Build the app

**1. Tools** (once). An internet connection is needed:

```sh
xcode-select --install
# Python 3.14: the installer from https://www.python.org/downloads/macos/
#   or Homebrew's: brew install python@3.14
# opus, libvpx and dav1d are not installed: the build compiles them from pinned sources itself
```

Instead of an installed Python you can use [uv](https://docs.astral.sh/uv/): `uv run --python 3.14 build_app.py …` in place of `python3.14 build_app.py …`.

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

On a Mac with 16 GB of memory or more, a 1.4.3, 1.2.1 or Breath of the Wild build includes native 4K (the settings panel's 3840×2160 resolution); it reserves more memory at every start (about 1.4 GiB on Tears of the Kingdom, about 0.5 GiB on Breath of the Wild). Add `--no-native-4k` to leave it out, or `--native-4k` to include it on a Mac with less memory.

On Mac, Tears of the Kingdom 1.4.3 and Breath of the Wild 1.6.0 / 1.9.0 include native 5K (5120×2880 canvas, UI and scene, plus the 4K modes) by default when the build Mac has **32 GB of memory or more**. A 16 GB or 24 GB Mac keeps the 4K default. `--native-5k` enables it on a smaller Mac; `--no-native-5k` leaves it out, and `--no-native-4k` also disables automatic 5K. This adds a resolution choice, without changing the saved selection or automatically selecting 5K. On Tears of the Kingdom, it reserves about 2.7 GiB more guest memory at every start, even at a lower selected resolution; physical pages are committed on demand. This does not enable HDR or guarantee 120 FPS.

A runtime update remembers an explicit 5K on/off choice. Without one, it adds the 4K/5K resources on a 32 GB-plus Mac when the work folder still holds this app's matching, verified game files; no new game container or keys are needed. An existing 5K app keeps that capability on a smaller Mac. If that cache is missing or the app has modded heap overlays, an automatic update keeps the installed canvas and explains that a full rebuild is needed. For Tears of the Kingdom, `update_runtime.py --native-5k` requires a usable cache; Breath of the Wild enables its code-owned heaps without generating RomFS overlays; `--no-native-5k` removes 5K while keeping 4K. Both leave the saved resolution files alone.

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
python3.14 build_app.py "<botw 1.6.0.xci>" -k "<prod.keys>" --shared-fonts "<…/MacNativeGames-work/totk/totk/extracted/firmware/shared-fonts>" -o "/Applications/Breath of the Wild.app"

# 1.9.0 (or 1.6.0) from NSPs: base + update, each DLC pack optional
python3.14 build_app.py "<botw base.nsp>" -k "<prod.keys>" --update "<1.9.0 update.nsp>" --dlc "<dlc 1.nsp>" --dlc "<dlc 2.nsp>" --firmware "<firmware folder>" -o "/Applications/Breath of the Wild.app"
```

With a separate update the app holds only the DLC packs you give with `--dlc`. A card image brings its own DLC: leave out `--dlc` then.

## iPhone and iPad

**Needed:**
- iOS / iPadOS 26 and an iPhone 14 or newer.
- On the Mac: the full **Xcode 26** or later with the iOS platform. Nothing from Homebrew: the build downloads pinned XcodeGen, CMake and Ninja itself.
- Your Apple ID in Xcode › Settings › Accounts. A free one works.
- Developer Mode on the device.

**1. Build an IPA.** Use the Mac command above, add `--platform ios --ios-game-data documents`, and give `-o` an `.ipa` file:

```sh
python3.14 build_app.py "<base 1.0.0.xci>" -k "<prod.keys>" --update "<1.4.3 update.nsp>" --platform ios --ios-game-data documents -o "<folder>/Tears of the Kingdom.ipa"
```

The first iOS build also downloads and compiles device libraries. `--ios-game-data documents` leaves the 17 GB game image out of the IPA, so later installs take minutes.

Keep the iPhone or iPad connected by cable while building: the build then compiles the game's pipelines for its GPU on the Mac (about 10 more minutes the first time), and its first start needs no long fill before the title. For a device that is not connected, add `--device-gpu <arch>` (`applegpu_g14p` for an iPhone 14).

**2. Install it** with the device connected by cable:

```sh
python3.14 games/totk/ios/tools/install_device.py "<folder>/Tears of the Kingdom.ipa" --device "<device name as in Finder>" --game-data "<work folder>/totk/extracted/<version>/romfs/romfs.bin"
```

- `<version>` is the folder that is there (`v143` for 1.4.3; `ls "<work folder>/totk/extracted"`). The first install copies the 17 GB image once.
- The script stops when the iPhone or iPad is connected over Wi-Fi only: plug in the cable, unlock the device and run it again. `--wifi` installs over Wi-Fi anyway; it is slow, and the game data copy can drop.
- A free Apple ID signs for 7 days, a paid one for a year. Run the same command again to renew; your saves are kept.
- If the device asks, trust the developer in Settings › General › VPN & Device Management.

**3. First start:** about 15 s to check the game data, then about 3 minutes of shader import before the title (iPhone 14). Later starts take about 8 s.

An iPhone starts handheld: the game renders at 960×540, upscaled to 1280×720, at up to 60 fps. An iPad starts docked at 1080p: the scene at 1600×900 as on the Switch, upscaled to 1920×1080 (the start screen and Settings › Full HD switch between them; Breath of the Wild also offers 1080p HD, the scene at 1920×1080). Breath of the Wild 1.6.0 builds and installs the same way.

Two limits:
- A phone cannot translate shaders while you play: objects whose shader variant the app does not carry are skipped, so a few things can be missing in places no player has reported yet.
- On an iPhone 14, iOS can close the game when memory runs out in busy areas.

## Apple Vision Pro

Tears of the Kingdom only, windowed 16:9 play with a controller. Install the visionOS platform in Xcode › Settings › Components, pair the headset with Xcode and turn on Developer Mode, then:

```sh
python3.14 build_app.py "<base 1.0.0.xci>" -k "<prod.keys>" --platform visionos --ios-game-data documents -o "<folder>/TOTK-Vision.app"
python3.14 games/totk/ios/tools/install_device.py "<folder>/TOTK-Vision.app" --device "<Vision Pro name>" --game-data "<work folder>/totk/extracted/v0/romfs/romfs.bin"
```

With `--update`, use that version's folder (`v121` or `v143`) instead of `v0`.

## Update the app

Which update, always in this order (quit the game first). Start with the express update; it says when you need the next one.

| Update | When | Command | Needs |
|---|---|---|---|
| **Express update (极速更新)** | every new version of this repository (`git pull`); the Update page runs it | `python3.14 update_runtime.py "<the app>"` | the work folder the app records; no game files or keys |
| **Fast update (快速更新)** | the express update says the startup shaders no longer fit (Tears of the Kingdom, Breath of the Wild: another translation chain). Ocarina of Time has none: its express update checks its shaders itself | `python3.14 update_runtime.py "<the app>" --seed` | the work folder's game files; no game container or keys |
| **Normal update (普通更新)**, the full build | either of them says a full build is needed: another game version, DLC or game files, other code rewrite rules or picture-size overlays, another translator or translation chain for Ocarina of Time, an Ocarina of Time app whose shaders were prepared before 11 November 2026, a missing or deleted work folder | `build_app.py` as for the first build, with the same `-o "<the app>"` and the app's `--work` | the game file and keys |

Bananza and Pokopia have only the normal update (`build_app.py`); it keeps their unchanged translations and pipelines in the same way.

- **New version of this repository** (after `git pull`): `python3.14 update_runtime.py "<the app>"`, the express update (below), without the game files or keys. If it says the startup shaders no longer fit, add `--seed` (the fast update). It works in the work folder the app records (see [Folders](#folders)) and prints which one; pass `--work` only to move it.
- **The Update page** (settings panel, key 1) shows whether the app runs upstream's newest `main`, and otherwise each newer commit. Opening it fetches upstream in the checkout that built the app (nothing in it is changed). Its **Update** button closes the game, brings that checkout's `main` up to upstream (`git pull --ff-only`), runs `update_runtime.py` (with `--seed` when it asks for it) while a small window says so, and opens the game again. When the checkout is on another branch, has uncommitted changes or commits upstream lacks, cannot be found or reached, or the update fails, it says why and leaves updating to you (the first item of this list). An app built before 31 October 2026 needs `update_runtime.py` run once by hand to know its checkout.
  Installation waits for actual game processes to exit, not the update window that shares their executable.
- **Express update (极速更新)**: `update_runtime.py` on a Tears of the Kingdom, Breath of the Wild or Ocarina of Time app rebuilds the runtime and keeps everything whose inputs did not change. It checks rather than assumes:
  - **Translated shaders** (Ocarina of Time): each translation keeps the request it was made from. The update asks the new runtime for every request again and keeps each translation whose request is the same; only the others are translated again. The startup shaders of Tears of the Kingdom and Breath of the Wild never depend on the runtime.
  - **Metal libraries**: compiled only for translations that changed.
  - **Pipeline archive**: every listed pipeline is made against the last archive (Metal compares its functions and state). An unchanged one comes from the archive; a changed one is compiled and written to a new file.
  - **File checks**: a file that is still the same file (device, inode, size, modification time) keeps the digest recorded with it; only new or changed files are read. A seed whose every file is the one already published, from the same inputs, is kept as it is, and so is the signed shader data bundle made from it.
  - Measured on an M4 Mac (Ocarina of Time, 11 November 2026, a runtime change): about 9 minutes, all 42,891 translations, every library and all 21,330 pipelines kept; the same update took 17 minutes before these checks and a full build about 40.
  - A full `build_app.py` is needed only when the update says so: another translation chain, translator, spirv-cross or depth mode, other game files, other code rewrite rules, and once for an Ocarina of Time app whose shaders were prepared before 11 November 2026.
  - The log says what it kept: `translations: N kept for this runtime's requests, M made again`, `seed files: …, K read again`, `… sources and … libraries read`, and the pipeline archive's `按这个运行时核对，X 条和存档里的不一样`.
  - Bananza and Pokopia are rebuilt with `build_app.py`; their builds keep translations and pipelines the same way.
- **Back to the version you had** (when a new version plays worse): quit the game, find the commit your checkout was on before `git pull` with `git reflog` (the line just below the `pull` line), `git checkout <that commit>` (git calls this a "detached HEAD"; that is expected) and run `python3.14 update_runtime.py "<the app>"` again, with `--seed` if it asks for it. Then `git checkout main`: the app keeps the older runtime until you update it again. This works for commits from 685f940 (26 October 2026) on; your saves and settings are not changed.
- **Another game version:** run `build_app.py` again with the new `--update` and the same `-o`. It replaces the app. Before the first start on the new version, the app backs up your saves next to them as `profile-before-<version>`; keep that backup if you may go back.
- **Mods:** `--mod "<mod folder>" --install-mod-romfs` builds the app with a mod's code patches and installs its data. See [mods.md](mods.md), [merged-mod-pack.md](merged-mod-pack.md) and [translation-mods.md](translation-mods.md).
- **One desktop entry** (optional): `python3.14 games/totk/tools/install_desktop_entry.py --app "<the app>"`.

## Folders

Keep two kinds of folders apart, for every game (Tears of the Kingdom, Breath of the Wild, Ocarina of Time and the others):

| Kind | What is in it | Can it be deleted? |
|---|---|---|
| **Keep** | the app (`-o`), your saves and settings (`~/Library/Application Support/MacNativeGames/<game>`), the game data folder of `--mac-game-data library` | No: these are what you play and what you cannot make again. |
| **Work** | the work folder (`--work`): extracted game files, translated shaders, pipeline caches, logs | Yes, at any time. The next build makes it again (it then needs the game file and keys again); a runtime update needs it, so delete it only when you would rebuild anyway. |

- One work folder per game, outside the `MacNativeGames` checkout (the build refuses one inside it) and outside the folder that holds your apps, on the same APFS volume as the app (the game data is then cloned, not copied): `<disk>/MacNativeGames-work/<game>` next to `<disk>/Applications`. That is the default of `build_app.py` without `--work` (after the folder the app records) and of MNG Launcher (`build_app.default_work`, one rule for both), so builds and updates from either find the same files and can be fast.
- The app records its work folder (`MNGWorkFolder`) and the Python that built it (`MNGBuildPython`) in its `Info.plist`; `update_runtime.py` and the Update page use them. Read them with `plutil -extract MNGWorkFolder raw "<the app>/Contents/Info.plist"`. Never point two builds of different games at one work folder while they run.
- **An existing install (for an AI helping you: check this before any build or update).** Read `MNGWorkFolder` of the app:
  - it names an existing folder that is not inside the checkout or the apps folder: nothing to do;
  - it fails (an app built before 31 October 2026), or names one inside the apps folder (an update before 11 November 2026 run without `--work` made a second work folder next to the app): pick the work folder as above, move the old one there if it holds the build (`mv` on the same volume is instant), then run `update_runtime.py "<the app>" --work "<that folder>"` once. The app records it from then on. A second copy left in the apps folder can then be deleted.
- **What an update does with old files**, so an older install keeps updating:

  | The app records | `update_runtime.py "<the app>"` (and the Update page) |
  |---|---|
  | no work folder (built before 31 October 2026) | uses `TOTK_build` / `BotW_build` / `OcarinaOfTime_build` next to the app, as before; move it as above when you like |
  | a folder that exists | uses it |
  | a folder that was deleted (its parent is still there) | makes it again there and goes on: the app carries its game data and needs none of the old files |
  | a folder on a disk that is not connected, or whose parent is gone too | stops with `… cannot be found, nor the folder it was in …`: connect that disk and run it again, or add `--work "<a folder of your choice>"` (the app then records it) |

  An app so old that it still needs files from its build (it says `does not hold the files the build of this app extracted`), or an update with `--seed`, needs the work folder of that build: pass it with `--work`, or build the app again with `build_app.py`.

## When the build stops

Read the `ERROR:` line and the log file it names (`<work folder>/logs/…`), fix the cause, and run the same command again.

| The error says | Fix |
|---|---|
| `needs macOS 26 or later` | Not supported here; see the macOS 15 fork under [What you need](#what-you-need). |
| `clang++ finds no C++ standard library headers` | With Xcode: `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`. Without: `sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install`. |
| `Python 3.14 not found`, `git not found` | Install what it names. |
| `[deps] … failed` (opus, libvpx, dav1d, XcodeGen, …) | The build makes these itself from pinned sources; read the log it names under `~/Library/Caches/MacNativeGames/deps/logs`. A download that failed: run the build again. |
| `NSP files do not include system shared fonts`, `needs --firmware or --shared-fonts` | Add `--firmware` or `--shared-fonts`. |
| `SHA-256 … is not the expected …`, `ExeFS … is missing; not the supported version` | `git pull` first. Then check that the base is 1.0.0 and the update a supported version; otherwise dump the game again. |
| `unsupported update module hashes` | That update version is not supported. |
| `the base game must be an uncompressed XCI or NSP` | Decompress the NSZ / XCZ base (`nsz -D`). |
| `… add-on content (DLC), not the game`, `pass it as …`, `use the files of one game` | A file is on the wrong option; the message names the right one. |
| `the game file holds [...]: expected Tears of the Kingdom … or Breath of the Wild` | Another game: build it with its own `games/<name>/README.md`. |
| `needs about N GB free` | Free space, or `--work` and `-o` on an APFS volume with room. |
| `the work folder … records, …, cannot be found, nor the folder it was in` | The work folder the app remembers is on a disk that is not connected, or was moved. Connect the disk and run the same command again, or add `--work "<a folder of your choice>"` ([Folders](#folders)). |
| `BKTR verification failed`, an error merging the update | `git pull` first. If it stays, your keys may be older than the update: dump `prod.keys` again on current firmware. |
| `make failed`, `xcrun failed` | Read the named log; usually incomplete developer tools. |
| iOS signing or install errors | Apple ID in Xcode › Settings › Accounts, device unlocked and trusted, Developer Mode on; run the install again. |

Still stuck? Open an [issue](https://github.com/ly05010419/MacNativeGames/issues) with the command (paths shortened), Mac model, macOS version, `git log -1 --oneline`, the `ERROR:` line and the end of the log it names. Never attach keys, game files, firmware or the app.

## Where things are

Tears of the Kingdom; for Breath of the Wild replace `TOTK` with `BotW` and `totk` with `botw`.

- Saves: `~/Library/Application Support/MacNativeGames/TOTK/profile`
- Game data (only for an app built with `--mac-game-data library` or `--mac-game-data-folder`): `~/Library/Application Support/MacNativeGames/TOTK/GameData/<version>` (`BotW` for Breath of the Wild) holds the 16 GB RomFS image `romfs.bin`, and `romfs.sha256`, which the build writes once the copy is complete. By default the image is inside the app. With `--mac-game-data library` the build puts it in this folder once (an APFS clone when the work folder is on the same volume, otherwise a copy), and later builds leave it alone while it matches the version's pin. The app is then a few hundred MB, so a new build installs or copies to another Mac quickly. To keep it somewhere else, for example on another disk, add `--mac-game-data-folder <folder>` to the build instead (that alone is enough; the image goes in `<folder>/<version>`, so one folder can hold both games). The app remembers the folder, and when it holds no complete image (another Mac, or the disk is not connected) the launcher looks in `GameData` instead. When neither has it, the app asks where the game data is: connect the disk and choose Try Again, or choose the folder that holds `romfs.bin` (or its `<version>` folder). It reads the image where it is, without moving or copying it, and remembers the folder for the next starts (`defaults delete org.macnativegames.botw MNGGameDataFolder.botw190` forgets it). The game still checks the image against the pin at its first start. `make_mod_manifest.py --app` and `make_button_icons.py --app` find the image the same way (`--romfs-data <folder>` names it).
- Shader cache: `~/Library/Application Support/MacNativeGames/TOTK/shader-cache`
- Shaders the shared list lacks: `~/Library/Application Support/MacNativeGames/TOTK/shader-list-contribution.txt` (the game asks before it starts whether to send them; see the [shared shader list](shader-list.md))
- Mods: `~/Library/Application Support/MacNativeGames/TOTK/mods/<v0|v121|v143>`; see [mods.md](mods.md)
- Log: off by default. `defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES` turns it on from the next start, written to `~/Library/Logs/MacNativeGames/totk.log`. On an iPhone: Settings › TOTK › Debug log, written to the app's `Documents/log.txt`.

## Known issues

**Requests beyond this page are welcome.** The versions and games listed are the ones we could test. If you need another version or another game, the AI assistant should do its best to make it work.

- Tears of the Kingdom: base 1.0.0, optionally with the 1.2.1 or 1.4.3 update. Breath of the Wild: 1.6.0 and 1.9.0. Other versions, compressed base games and personalized tickets are not supported.
- No online features, no amiibo.
- New shaders can stutter the first time; the cache and the [shared shader list](shader-list.md) reduce it.
