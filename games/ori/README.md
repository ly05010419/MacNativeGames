# Ori and the Blind Forest: Definitive Edition (community port)

Ori and the Blind Forest: Definitive Edition **1.0.1** running natively on Apple silicon Macs, built from **your own
copy** of the game. The game's ARM64 code (Unity / IL2CPP) is rewritten ahead of time and runs directly on the Mac's
CPU: no interpreter, no JIT, no CPU emulator. System services are answered by a small native host, and the Switch's NVN
graphics are rebuilt on Metal, with the game's GPU shaders translated to Metal.

**No game content is in this repository.** You need a dump of your own game and keys from your own Switch.

This folder carries its own copy of everything it builds and runs with (runtime, graphics layer, rewriter, shader
translator, scripts), forked from `games/totk` through the Luigi's Mansion 3 port; nothing outside `games/ori` is read.

## Status (25 October 2026)

Tested on a MacBook Pro M3 Max (36 GB, macOS 26.4); a packaged app also ran on a MacBook Air M1.

| Works | Details |
|---|---|
| Start, title, menus, saves | the boot loading screen, the title menu, save slots |
| Gameplay | played from the start at 16.7 ms per frame (60 fps), with sound and keyboard or controller |
| Picture | Unity renders 1920x1080 (docked) whatever the window; MetalFX scales it to the window (`TOTK_MFX_UPSCALE=0`: bilinear) |
| Shaders | translated in the background and kept in the shader cache; the app carries your own play's shaders (below) |

| Not done or not verified | Details |
|---|---|
| Online | answered as a console without a connection (Xbox Live sign-in, sockets, ssl contexts fail as offline) |
| Whole game | not played to the end |
| iPhone / iPad | no build |

## Build

Needs a Mac with Apple silicon and macOS 26 or later, the Xcode command line tools (Xcode's Metal toolchain,
`xcodebuild -downloadComponent MetalToolchain`, to compile shaders ahead of time; optional), Python 3.14, and from
Homebrew `opus libvpx`.

```sh
cd games/ori
python3.14 scripts/build_ori.py --game "<your card image>.xci" -k "<your prod.keys>" --firmware "<unpacked firmware NCA folder>"
```

- The game file is the card image (`010061D00DB74000`) that holds the 1.0.1 update; a base game without it takes
  `--update <1.0.1 update>.nsp`. The system fonts come from `--firmware` (an unpacked firmware NCA folder), or from the
  card's system update when it has one (not every Ori card does). The code is checked against
  `assets/expected-v101.json` (hashes only).
- Game data stays in this folder (`extracted/`, `rewritten/`, `build/`): never share or commit it; git ignores it,
  keys and apps here. A second build reuses every finished step.
- The app, `Ori and the Blind Forest.app`, goes next to the game file (`--out` to choose) and carries everything it runs
  with. Saves, the shader cache and the log are in `~/Library/Application Support/MacNativeGames/Ori` and
  `~/Library/Logs/MacNativeGames/ori.log`.

**Shaders on a new Mac.** Building the app after playing puts the shaders and pipelines your play collected into it
(`build/shader-seed`, compiled ahead of time to `build/shader-libs` with the Metal toolchain, plus a title-screen list
the first frame waits for). Another Mac then imports and warms them at its first start (the game waits at its first
frame until the warm-up ends) instead of translating them while you play. Areas not played yet are still translated
the first time.

## What it needed beyond the Luigi's Mansion 3 runtime

- **Rewriter** (`third_party/d2r/patch_nso.py`): Unity's code uses x18 as an ordinary register; 4 indirect tail calls
  through x18 (`site_br_tail`) and 4 signed jump tables in other shapes (`signed_table_split`).
- **Threads**: SVC 0x32 / 0x33 (`SetThreadActivity` / `GetThreadContext3`) for Unity's garbage collector, which stops
  every thread: one parked in an SVC, or in guest code through SIGUSR2 with its full register state.
- **Memory**: `QueryMemory` reports the runtime's tail of each module as CodeData, so the SDK's unwinder handles the
  C++ exceptions IL2CPP throws for managed ones.
- **Services**: 13 more `nn::fs` calls on the host (RAM disk, add-on content, directories, free space), RomFS
  directory listing, CMIF domains, and offline answers for `bsd:u`, `nifm`, `ssl`, the socket resolver, the profanity
  filter, `csrng`, `set` and the controller setup calls.
- **NVN**: shader control sections copied at `nvnProgramSetShaders` (Unity frees them right after),
  `nvnTexturePoolRegisterImage`, present interval 0 without waiting for the display (the loading screen), NVN 53.399
  reported (the game traps unless minor > 311).
- **Shader translator**: the compute shared-memory size (control+0x754) is passed to the translator; without it, the
  bloom kernels read garbage at tile edges (bands every 75 rows).
- **Start**: the first frame waits until the shader warm-up ends, so the game does not play its intro unseen behind the
  progress screen.

No patch to the game's code: it runs as shipped.
