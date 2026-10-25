# Super Smash Bros. Ultimate native Mac port

Super Smash Bros. Ultimate **13.0.2** running natively on Apple silicon Macs, built from **your own copy** of the game.
The game's own ARM64 code (the program and the 98 NRO modules for fighters, stages and modes) is rewritten ahead of time
and runs directly on the Mac's CPU: no interpreter, no JIT, no CPU emulator. System services are answered by a small
native host, and the Switch's NVN graphics are rebuilt on Metal 4, with the game's GPU shaders translated to Metal.

**No game content is in this repository, and none will ever be.** You need a legal dump of your own cartridge and its
update, and keys from your own Switch. We do not help anyone obtain keys or game files.

This folder carries its own copy of everything it builds and runs with (runtime, graphics layer, rewriter, shader
translator, scripts); nothing outside `games/ssbu` is read. How the port was done and what was found on the way:
[CHANGELOG.md](CHANGELOG.md).

## Status (24 October 2026)

Tested on one Mac: MacBook Pro, Apple M1 Max, 64 GB, macOS 27.

| Works | Details |
|---|---|
| Start, title, menus | intro movie with sound, all main menus, options |
| Fights | Smash with several fighters and stages, items; keyboard and game controllers |
| World of Light (adventure) | map, spirit battles, the Spirit Board, saving and loading |
| Saves | autosave and reload; the save picture of World of Light |
| Language | changing it in the game's options restarts the game, as on the console |
| Picture | 1080p as shipped or 2K (2560x1440), chosen before the start (app: Picture menu); MetalFX can upscale to the window |
| Sound and movies | game audio (hardware Opus), VP9 movies with Vorbis sound |

| Not done or not verified | Details |
|---|---|
| Online | answered as a console without an internet connection; the online menu closes again |
| Frame interpolation (key 2) | creates no frames in this game (the host lacks the game's camera, depth and UI) |
| Album | saving a screenshot to the Switch album is not answered yet |
| Known picture gaps | the alpha test of a few menu graphics is not applied; fighters were seen black once in a while (cause not found yet) |
| iPhone / iPad | no build: the game loads code modules while it runs, which iOS does not allow |
| Labo VR | not available on the Mac |

## Build

Needs a Mac with Apple silicon and macOS 26 or later, Xcode with the Metal toolchain
(`xcodebuild -downloadComponent MetalToolchain`), Python 3.14, and from Homebrew `opus libvpx libvorbis libogg`.

```sh
cd games/ssbu
python3.14 scripts/build_ssbu.py --game "<your card image>.xci" --update "<your 13.0.2 update>.nsp" --keys "<your prod.keys>"
```

- The game file is the 1.0.0 card image (`01006A800016E000`, XCI); a base NSP works with `--firmware <unpacked firmware
  NCA folder>` for the system fonts. The update is 13.0.2 (`01006A800016E800`), whose code the app runs. Every input is
  checked against `assets/expected-v13.0.2.json` (hashes only).
- The first build takes a while (the 17.6 GB RomFS, the rewrite, and the startup shaders from data.arc); a second build
  reuses every verified step.
- The work folder is `games/ssbu/SSBU_build` (`--work` to choose another). It holds game data: never share or commit
  it. Git ignores it, game files, keys and apps in this folder.
- The app goes next to the game file (`--out` to choose). Saves, the shader cache and the log are in
  `~/Library/Application Support/MacNativeGames/SSBU` and `~/Library/Logs/MacNativeGames/ssbu.log`; a build never
  touches them.

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

## Shaders

The app carries every shader of data.arc and every variant of the shared list (`assets/shader_list.tsv`: shader keys and
bind states only, no game data; [docs/shader-list.md](../../docs/shader-list.md)), compiled into Metal libraries, so a
new stage or fighter does not translate while it draws. What your own game meets that the list lacks is noted on your
Mac and built in by your next build; nothing is sent anywhere.

## Developer tools

- `scripts/gap_report.py`: what the host reported it could not do yet, from its own logs (`--session <log>` for one).
- `scripts/override_audit.py`: every SDK function the host replaces, checked against the original's return type in the
  SDK and how the game reads the answer.
- `tests/`: probes of the input and graphics code, run like the other games' tests.
