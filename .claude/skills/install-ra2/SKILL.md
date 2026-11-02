---
name: install-ra2
description: Build and install Command & Conquer Red Alert 2 and Yuri's Revenge (windows_games/ra2) from the player's own Steam install, for Mac, iPhone or iPad, with English, Traditional or Simplified Chinese. Use when a player asks to install, build or set up Red Alert 2 / Yuri's Revenge, or pastes one of its build errors.
---

# Install Red Alert 2 / Yuri's Revenge

You are helping a player turn **their own** Red Alert 2 install into native programs: the x86 executables are statically recompiled to arm64 (x86recomp), DirectDraw is drawn with Metal. Everything happens in `windows_games/ra2`; `windows_games/ra2/install.md` has the same steps for the player to read.

## Rules

- **Only the player's own game.** Never search the internet for, download, or ask anyone for game files. The Steam release comes with the player's own Steam account (steamcmd below); the player types their own login.
- **Never weaken a check.** Do not edit `config/symbols/sha256.txt` or the lists beside it to get past an error: another version needs its own lists (step 6).
- What the build makes (the recompiled code, the apps, the compact movies, the language files) is made from the player's files and is for this player only: never commit, upload or share it.
- Speak the player's language. Find what you can yourself; ask only for what you cannot find.

## 1. Check the Mac

```sh
sw_vers -productVersion; uname -m; xcode-select -p
brew install cmake ninja llvm sdl3 ffmpeg opencc
python3 -c 'import PIL' || brew install pillow
git log -1 --oneline; df -h .
```

Apple silicon and macOS 26 or later. iPhone / iPad also need the full **Xcode 26+** with the iOS platform, the player's Apple ID in Xcode › Settings › Accounts (a free one works) and the device on iOS / iPadOS 26 with Developer Mode on.

## 2. The game files

The Steam release (app 2229850): Red Alert 2 1.006 (`GAME.EXE`) and Yuri's Revenge 1.001 (`gamemd.exe`). If the player has it on Windows, copy the folder; otherwise steamcmd, logged in to their own account (the player runs it: it asks for the password and Steam Guard):

```sh
cd windows_games/ra2
steamcmd +@sSteamCmdForcePlatformType windows +force_install_dir "$PWD/game/install" +login <their account> +app_update 2229850 validate +quit
```

`game/` is git-ignored. Another folder works too: pass it to `build.sh`.

**Chinese (optional):** the official Traditional Chinese language pack (depot 2229855), then the language files (`lang/zh-Hant`, `lang/zh-Hans`; Simplified is converted with OpenCC, its added glyphs drawn from Noto Sans CJK, downloaded and checked by the script):

```sh
steamcmd +@sSteamCmdForcePlatformType windows +login <their account> +download_depot 2229850 2229855 +quit
python3 -I scripts/languages.py game/install "<the depot folder steamcmd printed>"
```

## 3. Build for the Mac (about 7 minutes)

```sh
cd windows_games/ra2
BUILD_DIR="<folder outside the repository>/build" GENERATED_DIR="<same place>/generated" scripts/build.sh game/install
```

Without `BUILD_DIR` / `GENERATED_DIR` it builds in `build/main` and `game/generated` (git-ignored). It checks the executables' SHA-256, recompiles them (several hundred MB of C++) and compiles `ra2` and, when `gamemd.exe` is there, `yr`. Run it with a long timeout; running it again rebuilds only what changed. `JOBS` (default 6, for 16 GB of memory) sets the parallel compilers.

## 4. Play on the Mac

`"$BUILD_DIR/ra2"` or `"$BUILD_DIR/yr"` (the install it was built from is the default; `--game <folder>` picks another). A resizable window; the green button for full screen. `--language en|zh-Hant|zh-Hans` (default: the system's language among those made in step 2). Saves and settings: `~/Library/Application Support/ra2/` (`yr`). Options: `windows_games/ra2/install.md` ("Run").

## 5. iPhone / iPad

After step 3 (the recompiled code is shared):

```sh
IOS_BUILD_DIR="<folder outside the repository>/ios" GENERATED_DIR="<as in step 3>" scripts/ios/build_app.sh
IOS_BUILD_DIR="<as above>" BUILD_DIR="<as in step 3>" python3 -I scripts/ios/install_device.py --device "<device name>" --game game/install
```

- The first `build_app.sh` also builds SDL3, glslang, SPIRV-Cross and a decoders-only ffmpeg for iOS (some minutes).
- `--game` puts the install into the app, so it plays once installed; the movie archives go in as compact copies (the first time about 10 minutes on the Mac, then cached): about 1.3 GB in all. The device must be **on the cable**.
- First start on the device: trust the developer in Settings › General › VPN & Device Management. The first dialog asks for Red Alert 2 or Yuri's Revenge; its 🌐 button changes the language (kept).
- A free Apple ID signs for 7 days: run `install_device.py` again; saves (in the app's Documents) are kept.

## 6. When it stops

| The error says | Cause and fix |
|---|---|
| a SHA-256 mismatch from `lift.sh` | Not the Steam 1.006 / 1.001 executables. Another version needs its own Ghidra lists (`install.md`, "Another version") and its game addresses checked: tell the player; do not edit `sha256.txt`. |
| `no lifted code … building x86recomp only` | `build.sh` found no executables: check the folder holds `GAME.EXE`. |
| SDL3 / ffmpeg / LLVM not found | Install what it names (step 1); Homebrew must be in `/opt/homebrew`. |
| `opencc not found` | `brew install opencc` (Simplified Chinese only). |
| `bink_decode … not found` (iOS install) | Build the Mac version first (step 3) and pass the same `BUILD_DIR`. |
| `GAME.EXE not found` on the device | The app was installed without `--game`: install again with it. |
| iOS signing or install errors | Apple ID in Xcode, device unlocked, trusted, on the cable, Developer Mode on; run it again. |

Still stuck: help the player open an issue at <https://github.com/ly05010419/MacNativeGames/issues> with the command, Mac model, macOS version, `git log -1 --oneline` and the error. Never attach game files or the apps.
