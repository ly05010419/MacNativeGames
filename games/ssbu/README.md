# Super Smash Bros. Ultimate native Mac port

Super Smash Bros. Ultimate **13.0.2 or 13.0.5** running natively on Apple silicon Macs, built from **your own copy** of
the game. The game's own ARM64 code (the program and the 98 NRO modules for fighters, stages and modes) is rewritten ahead
of time and runs directly on the Mac's CPU: no interpreter, no JIT, no CPU emulator. System services are answered by a
small native host, and the Switch's NVN graphics are rebuilt on Metal 4, with the game's GPU shaders translated to Metal.

**No game content is in this repository, and none will ever be.** You need a legal dump of your own cartridge and its
update, and keys from your own Switch. We do not help anyone obtain keys or game files.

This folder carries its own copy of everything it builds and runs with (runtime, graphics layer, rewriter, shader
translator, scripts); nothing outside `games/ssbu` is read. How the port was done and what was found on the way:
[CHANGELOG.md](CHANGELOG.md).

## Status (26 October 2026)

Tested on one Mac: MacBook Pro, Apple M1 Max, 64 GB, macOS 27. Sessions of 1-2 hours on 2K in World of Light and
Versus run without a crash (after the first 10 minutes of warm-up, 0.1-0.8 % dropped frames per 10 minutes; thermal
state nominal).

13.0.5 (the final update, September 2026: a fix for online battles only) builds next to it, tested on 27 October on an
Apple M5, 32 GB, macOS 27.2, from a base NSP and the 13.0.5 update NSP: matches at a steady 60 Hz in a 3420x1916
window, with MetalFX upscaling on and off, and the start in 2K with every render size site checked and applied. Its
fighters, stages and modes are the 13.0.2 ones (the same 98 NRO modules), and so is everything but `main` (see
[CHANGELOG.md](CHANGELOG.md)).

| Works | Details |
|---|---|
| Start, title, menus | intro movie with sound, all main menus, options |
| Fights | Smash with several fighters and stages, items; keyboard and game controllers |
| World of Light (adventure) | map, spirit battles (Giga Bowser included), the Spirit Board, saving and loading |
| DLC fighters | your own dumps of the DLC you bought: choose their folder in the app's DLC menu (or `--dlc` at build time); the game announces the new fighters |
| Saves | autosave and reload; the save picture of World of Light |
| Photos | a photo taken in a fight is saved as a PNG on the Mac (app: Photos menu) |
| Language | changing it in the game's options restarts the game, as on the console |
| Picture | 1080p as shipped or 2K (2560x1440), chosen before the start (app: Picture menu); MetalFX can upscale to the window |
| Sound and movies | game audio (hardware Opus), VP9 movies with Vorbis sound |
| Logs | every session's log is kept (newest 40, at most 1 GB); the app's Log menu turns the game log off |

| Not done or not verified | Details |
|---|---|
| Online | answered as a console without an internet connection; the online menu closes again |
| Frame interpolation (key 2) | creates no frames in this game (the host lacks the game's camera, depth and UI) |
| Album | photos go to a Mac folder; the game's own album list stays empty |
| Known picture gaps | fighters were seen black once in a while in early builds, not since (cause not found) |
| iPhone / iPad | no build: the game loads code modules while it runs, which iOS does not allow |
| Labo VR | not available on the Mac |

## Build

Needs a Mac with Apple silicon and macOS 26 or later, Xcode with the Metal toolchain
(`xcodebuild -downloadComponent MetalToolchain`), Python 3.14, and from Homebrew `opus libvpx libvorbis libogg`.

```sh
cd games/ssbu
python3.14 scripts/build_ssbu.py --game "<your card image>.xci" --update "<your 13.0.2 or 13.0.5 update>.nsp" --keys "<your prod.keys>"
```

- The game file is the 1.0.0 card image (`01006A800016E000`, XCI); a base NSP works with `--firmware <unpacked firmware
  NCA folder>` for the system fonts. The update is 13.0.2 or 13.0.5 (`01006A800016E800`), whose code the app runs; the
  build tells them apart by the update's verified ExeFS, never by the file name. Every input is checked against
  `assets/expected-v13.0.2.json` or `assets/expected-v13.0.5.json` (hashes only). An NSZ update is decompressed first
  (`nsz -D`).
- The first build takes a while (the 17.6 GB RomFS, the rewrite, and the startup shaders from data.arc); a second build
  reuses every verified step.
- The work folder is `games/ssbu/SSBU_build` (`--work` to choose another). It holds game data: never share or commit
  it. Git ignores it, game files, keys and apps in this folder.
- The app goes next to the game file (`--out` to choose). Saves, the shader cache and the log are in
  `~/Library/Application Support/MacNativeGames/SSBU` and `~/Library/Logs/MacNativeGames/ssbu.log`; a build never
  touches them.
- `--dlc <folder with your own dumps of the DLC you bought>` builds that list into the app (every package verified
  with your keys); later builds keep it.
  The app's DLC menu does the same without a rebuild.

## Play

Keys as in Tears of the Kingdom: WASD the left stick, arrow keys the right stick, J is A, K or Space is B, I is X, U is Y,
Q / E are L / R, Return is +. Game controllers recognized by macOS work as a Pro Controller. The **Picture** menu sets
1080p or 2K for the next start (Smash makes its render images once, at start).

| Key | What it does in Smash |
|---|---|
| 1 | overlays: all, FPS only, off (the FPS line names the picture size and MetalFX) |
| 2 | frame interpolation: makes no frames in this game (see above) |
| 3 | MetalFX upscales the finished picture to the window, on or off |
| 4 | aspect ratio of the picture in the window |
| 5 | edge smoothing (MetalFX Temporal), on or off |
| 6 | controller type: auto or a fixed one |
| 7 | mouse mode on or off |

The app's menus (in the menu bar while the game runs):

| Menu | What it does |
|---|---|
| Picture | 1080p or 2K for the next start |
| Log | the game log on or off for the next start (off: nothing is written; the session archive stays) |
| DLC | your DLC on or off, and Choose DLC Folder... for the folder with your own dumps of the DLC you bought; it says how many DLC and fighters it found, and they apply at the next start |
| Photos | where the photos you take in a fight go (`~/Pictures/Super Smash Bros. Ultimate` unless you choose a folder), and Show in Finder |

## Shaders

The app carries every shader of data.arc and every variant of the shared list (`assets/shader_list.tsv`: shader keys and
bind states only, no game data; [docs/shader-list.md](../../docs/shader-list.md)), compiled into Metal libraries, so a
new stage or fighter does not translate while it draws. What your own game meets that the list lacks is noted on your
Mac and built in by your next build; nothing is sent anywhere.

## Developer tools

- `scripts/gap_report.py`: what the host reported it could not do yet, from its own logs (`--session <log>` for one).
- `scripts/override_audit.py`: every SDK function the host replaces, checked against the original's return type in the
  SDK and how the game reads the answer.
- `tests/`: probes of the input and graphics code, run like the other games' tests, and Python tests of the DLC
  reader, the gap report, the override audit and the shader seed (`python3 -m unittest discover -s games/ssbu/tests`
  from the repository root; the override audit's real-SDK test needs the work folder and is skipped without it).
