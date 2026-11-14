# Installing Tears of the Kingdom on Apple Vision Pro

MacNativeGames runs *The Legend of Zelda: Tears of the Kingdom*'s own ARM64 code directly on Apple chips, with its NVN graphics rebuilt on Metal. On Apple Vision Pro it plays in a 16:9 window with a game controller (early support; it isn't VR). This guide covers only the Vision Pro build; the Mac, iPhone and iPad builds are in [docs/install.md](docs/install.md).

## Requirements

- A Mac with Apple silicon and macOS 26 or later (16 GB of memory or more recommended)
- The full Xcode 26 or later, with the visionOS platform (Xcode › Settings › Components)
- Apple Vision Pro on visionOS 26, in Developer Mode and paired with Xcode
- An Apple ID in Xcode › Settings › Accounts (a free one works; the app then needs re-installing every 7 days, a paid one lasts a year)
- Python 3.14 (from python.org, or `brew install python@3.14`), or [uv](https://docs.astral.sh/uv/)
- A supported game controller
- About 45 GB free on the Mac (60 GB with an update), on an APFS volume

## Game files

You need to supply your own game files. This repository contains none. Use only files dumped from a game you bought and from your own Switch.

1. Dump the **base game 1.0.0** (title ID `0100F2C0115B6000`) as an XCI or NSP. It must be uncompressed: decompress an NSZ or XCZ first (`nsz -D`).
2. Optionally dump an update, **1.2.1 or 1.4.3** (title ID `0100F2C0115B6800`). 1.4.3 is the most tested.
3. Dump **`prod.keys`** from the same Switch. A keys file older than the update can't open it.
4. If the base game is an NSP, also give your firmware dump (`--firmware <folder of .nca files>`) for the system fonts. An XCI carries them itself.

The build checks the files themselves (the title ID is read from the file, not its name) and verifies the extracted data.

## Build

Start from a checkout of this repository (`git pull` first if it's an older one). Put each path in quotes:

```sh
python3.14 build_app.py "<base 1.0.0.xci>" -k "<prod.keys>" --platform visionos --ios-game-data documents -o "<folder>/TOTK-Vision.app"
```

Add `--update "<1.4.3 update.nsp>"` for an update, and `--firmware "<firmware folder>"` for an NSP base. The first build takes 20 to 40 minutes and downloads and compiles pinned dependencies; let it run to the end. If it's interrupted, run exactly the same command again. It's done when the last line says `done in N min: <path of the app>`.

`--ios-game-data documents` leaves the 17 GB game image out of the app; the install step copies it to the headset once.

## Install on Apple Vision Pro

With the headset paired with Xcode and unlocked:

```sh
python3.14 games/totk/ios/tools/install_device.py "<folder>/TOTK-Vision.app" --device "<Vision Pro name>" --game-data "<work folder>/totk/extracted/v0/romfs/romfs.bin"
```

- The work folder is `~/MacNativeGames-work/totk` unless you passed `--work`. With an update, use that version's folder (`v121` or `v143`) instead of `v0`.
- The script signs the app with your Xcode team and copies the game image into the app's Documents once; a later install skips it while the headset holds the same image.
- Run the same command again to renew the signature. Your saves and shader cache are kept.
- If the headset asks, trust the developer in Settings › General › VPN & Device Management.

## Controls and known issues

- Play with a game controller in a 16:9 window. Breath of the Wild has no Vision Pro build yet.
- Vision Pro support is listed as early. Controls and other known issues are in [docs/controls.md](docs/controls.md) and [docs/install.md](docs/install.md#known-issues).
