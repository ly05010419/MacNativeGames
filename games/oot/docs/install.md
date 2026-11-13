# Install Ocarina of Time

[← README](../README.md) · [中文](install.zh-CN.md)

This page only covers turning **your own** dump of The Legend of Zelda: Ocarina of Time for Nintendo Switch 2 into an app on your Mac. A build takes about half an hour, most of it waiting, plus the first copy of the 26 GB game data.

Ground rules: use only your own console and your own game. Nothing is downloaded for you, and nothing from the game is in this repository. Never edit `games/oot/assets/expected-v131072.json` or the scripts to get past a check: the error message says what is wrong. The app you build contains the game: it is for you only.

Contents: [What you need](#what-you-need) · [Build the app](#build-the-app) · [First start](#first-start) · [Update the app](#update-the-app) · [Folders](../../../docs/install.md#folders) · [When the build stops](#when-the-build-stops) · [Where things are](#where-things-are)

## What you need

**A Mac** with Apple silicon and **macOS 26** or later (the graphics use Metal 4). 16 GB of memory or more is recommended.

**Your dump** of the game with the update **v131072**, title ID `0400486024DEC000`, in either form:
- a `.nup` file, or
- a dump folder holding `base/` and `updates/131072/` (each with `exefs/` and `romfs/`).

The build checks the code and the game data against `expected-v131072.json`; another version is refused. No `prod.keys` is needed: the dump holds the decrypted files.

**System fonts**: the six `.bfttf` files and their `manifest.json` that a Tears of the Kingdom or Breath of the Wild build of this repository extracted, in `TOTK_build/totk/extracted/firmware/shared-fonts` (or `BotW_build/botw/...`). Give that folder with `--shared-fonts`. This build has no `--firmware` option. After the first build the work folder keeps them, so later builds need no `--shared-fonts`.

**Tools** (once; an internet connection is needed):

```sh
xcode-select --install
# Python 3.14: the installer from https://www.python.org/downloads/macos/ (or brew install python@3.14)
# opus, libvpx and dav1d: the build compiles them from pinned sources; no llvm or ffmpeg
```

and the full **Xcode** app with its Metal toolchain (Xcode › Settings › Components › Metal Toolchain, or `xcodebuild -downloadComponent MetalToolchain`): the build compiles the game's shaders with Apple's offline Metal compiler, which the command line tools do not include. The build also installs three Python packages (`lz4`, `capstone`, `zbic`) into its own environment in the work folder.

**Free disk space** on the volume of the work folder: about 40 GB.

The work folder is `OcarinaOfTime_build` next to the dump. Keep it and the app on the **same APFS volume**: the game data is then cloned, not copied. Pass `--work <folder>` and `-o <app>` to put them on another APFS disk with room.

## Build the app

**1. This repository**: `git clone https://github.com/ly05010419/MacNativeGames.git` (or download the ZIP), then open Terminal in the `MacNativeGames` folder. If you already have it, run `git pull` first.

**2. Build.** Put each path in quotes. Pick the app's final place now with `-o`; without it the app is written next to the dump.

```sh
# from a .nup file
python3.14 build_app.py "<Ocarina of Time.nup>" --shared-fonts "<…/TOTK_build/totk/extracted/firmware/shared-fonts>" -o "/Applications/Ocarina of Time.app"


# from a dump folder (base/ and updates/131072/)
python3.14 build_app.py "<dump folder>" --shared-fonts "<…/shared-fonts>" -o "/Applications/Ocarina of Time.app"
```

If `/Applications` is on another volume than the work folder, choose a folder on the work folder's volume instead.

Optional: `--mac-game-data library` keeps the 26 GB game image out of the app, in `~/Library/Application Support/MacNativeGames/OcarinaOfTime/GameData` (or `--mac-game-data-folder <folder>` to name another folder). The app is then 5–6 GB and later builds install faster.

**3. Let it run to the end.** On an M4, a full build whose game files were already imported took about 31 minutes; the first build also copies and checks the 26 GB game data. The log names each step as it starts:

| Step | What happens |
|---|---|
| `import-original-nup` / `import-original-dump` | the game files are copied into the work folder and checked |
| `verify-original-modules`, `rewrite-…`, `verify-rewrite-…` | the game code is rewritten for Apple silicon and checked against the expected result |
| `native-runtime` | the runtime is compiled |
| `prepare-shader-seed` | every shader of the game (about 42,800) is translated to Metal |
| `compile-shader-libraries` | they are compiled for the GPU |
| `Preparing … pipelines` | the pipeline archive is built from the shader list |
| `Built <path of the app>` | done |

- Long quiet stretches are normal. Do not stop it.
- If it was interrupted, run **exactly the same command** again: finished steps are reused.
- Quit the game before building, and never run two builds on the same work folder.

## First start

Open the app from Finder. The game starts with its title movie and opening, as on the console. Key **1** opens the settings panel (resolution, upscaler, frame lock, frame interpolation; see the [README](../README.md#settings)). A place you visit for the first time can stutter briefly while pipelines no player has recorded yet are built.

## Update the app

- **New version of this repository** (after `git pull`): quit the game, then

  ```sh
  "$(plutil -extract MNGBuildPython raw "<the app>/Contents/Info.plist")" update_runtime.py "<the app>"
  ```

  from the `MacNativeGames` folder: the Python the app was built with (its virtual environment has the build's tools), and the update works in the work folder the app records (`MNGWorkFolder`) and prints which one. Pass `--work` only to move the work folder. An app that records none falls back to `OcarinaOfTime_build` next to it; see [Folders](../../../docs/install.md#folders) for where the work folder belongs and how to move an older install's. Settings → Update in the app runs the same update.

  It rebuilds the runtime from this checkout and then the pipeline archive from the warm-up lists the app carries (render, compute and mesh pipelines), and reuses the shaders, in a few minutes; after a runtime change the archive is filled again (about 3 minutes on an M4). When it says the shaders need the full build, run the `build_app.py` command of [Build the app](#build-the-app) again: the work folder keeps everything else.
- Your saves and settings are not changed by either.

## When the build stops

Read the `ERROR:` line and the log file in `<work folder>/logs/`, fix the cause, and run the same command again.

| The message says | Fix |
|---|---|
| `unsupported or damaged original ExeFS`, `RomFS is not update 131072: missing … / other size …` | Not update v131072, or an incomplete dump: dump the game again. The RomFS check goes file by file and names the files. Retail packages of 131072 differ a little: files the list does not name are fine, and from a `.nup` a listed file of the listed size with other bytes is fine too (the package's own hashes are checked; the build log names them). |
| `RomFS is not update 131072 as listed: different …` | Only from a dumped folder, which has no hashes of its own: those files may be another retail package's or damaged, and the build cannot tell. Build from the game's `.nup` instead. |
| `… compute pipeline(s) could not be built on this Mac (…)` | The Metal compiler of this Mac failed on the shaders it names (seen on Apple M5 with macOS 26.6.2, #2070). Update macOS if an update is offered and build again; if it still fails, open an issue with the shader key and the lines of `logs/compute-pipelines.log` it names. |
| `the pipeline archive was built from another pso_….tsv than the App carries`, `… planned N listed render/mesh pipelines, the App's lists name M`, `the pipeline archive holds none of the … listed mesh pipelines` | The archive and the app's warm-up lists do not match, so the app is not installed. Run the same command again; if it repeats, open an issue with the line and `logs/compute-pipelines.log`. |
| `use the complete original update 131072 in the dump/NUP`, `leave out --update, --dlc and --mod` | Give only the `.nup` or the dump folder; it holds its update. |
| `pass --shared-fonts with the six verified firmware fonts` | Add `--shared-fonts` (see [What you need](#what-you-need)). |
| `no Metal compiler (Xcode -> Settings -> Components: Metal Toolchain)` | Install the full Xcode and its Metal toolchain. |
| `the work folder … records, …, cannot be found, nor the folder it was in` | The work folder the app remembers is on a disk that is not connected, or was moved. Connect the disk and run the same command again, or add `--work "<a folder of your choice>"` ([Folders](../../../docs/install.md#folders)). A deleted work folder is made again by itself. |
| `… must remain outside the source checkout` | Put `--work` and `-o` outside the `MacNativeGames` folder. |
| `unfinished App staging directory exists; left untouched` | A build stopped while writing the app: delete the `.Ocarina of Time.app.ready` folder next to the app, then build again. |
| `this title currently uses its original rendering options` | Leave out `--native-4k`, `--native-5k` and `--vr`. |
| `game did not finish its normal close; App left untouched` | Quit the game, then build again. |

Still stuck? Open an [issue](https://github.com/ly05010419/MacNativeGames/issues) with the command (paths shortened), Mac model, macOS version, `git log -1 --oneline`, the `ERROR:` line and the end of the log it names. Never attach game files, fonts or the app.

## Where things are

- Saves: `~/Library/Application Support/MacNativeGames/OcarinaOfTime/profile`
- Settings: the `graphics-*.txt` files in the same folder
- Shader cache: `~/Library/Application Support/MacNativeGames/OcarinaOfTime/shader-cache`
- Shaders and pipelines the shared list lacks: `~/Library/Application Support/MacNativeGames/OcarinaOfTime/shader-list-contribution.txt`
- Game data, only with `--mac-game-data library`: `~/Library/Application Support/MacNativeGames/OcarinaOfTime/GameData`
- Log: off by default. `defaults write org.macnativegames.oot TOTK_DEBUG_LOG -bool YES` turns it on from the next start, written to `~/Library/Logs/MacNativeGames/ocarina.log`.
