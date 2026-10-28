# Mario Kart 8 Deluxe native Mac port (early)

Mario Kart 8 Deluxe **4.0.1** running natively on Apple silicon Macs, built from **your own copy** of the game. The game's
own ARM64 code is rewritten ahead of time and runs directly on the Mac's CPU: no interpreter, no JIT, no CPU emulator. System
services are answered by a small native host, and the Switch's NVN graphics are rebuilt on Metal 4, with the game's GPU shaders
translated to Metal. The runtime is this repository's own, copied from `games/odyssey` (see
[third_party/COPIES.md](third_party/COPIES.md)).

**No game content is in this repository, and none will ever be.** You need a dump of your own game and update, keys and
firmware from your own Switch. We do not help anyone obtain keys or game files.

## Status (28 October 2026)

Played by hand on a MacBook Pro with an Apple M5, 32 GB, macOS 27.2, from a base NSZ and the 4.0.1 update NSZ, with the app
full screen on the built-in display (3420x2146, 60 Hz).

| Works | Details |
|---|---|
| Races | the Triforce Cup, at 60 fps: in one app session of 51,600 frames, every 600-frame window had p50 and p95 frame intervals of 16.67 ms, and 0.11% of frames were shown a refresh late. The longest waits (up to 3 s) were frames that built new shaders, on a scene's first visit (see below) |
| Boot to the title screen | its demo race renders and plays with sound |
| Miis | the six default Miis and random Miis for the demo race; heads are empty (see below) |
| Saves | the device save and the account save are kept apart |
| Menus | single player, the mode menu, the cc select and Time Trial's character select, with their H.264 movie previews (VideoToolbox) |
| Shader seed | every shader of the game (29,717, as 30,795 variants) translated and compiled at build time; in a test start from an empty cache the game translated none in play and loaded all 601 libraries it used precompiled |

The game renders its own 1080p picture; the app scales it to the screen (key 3: MetalFX upscaling). The FPS line in the
top-right corner (`TOTK_FPS_OVERLAY=0` hides it) shows the frame rate and the size of the picture on screen.

Not yet: other cups and modes checked by hand, iPhone / iPad. The race numbers above are from before the shader seed.

## Building

You need an Apple silicon Mac with macOS 26 or newer, Xcode 27.1 (its Metal toolchain), Homebrew's `opus` and `libvpx`, and
Python 3.14 with this folder's requirements:

```sh
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python scripts/build_mk8d.py --game BASE.nsz --update UPDATE_4.0.1.nsz --keys prod.keys \
    --firmware FIRMWARE_NCA_FOLDER --work ~/mk8d-work
make app WORK=$HOME/mk8d-work      # Mario Kart 8 Deluxe.app in the work folder; or: make run WORK=$HOME/mk8d-work
```

`make app` builds **Mario Kart 8 Deluxe.app**, which starts full screen. Put it elsewhere with `APP=/Applications/Mario\ Kart\ 8\ Deluxe.app`;
on the same volume as the work folder its game files are APFS clones and take no extra space.
- Saves, the shader cache and logs go to `MK8D Data` in the work folder (`DATA=` to change it). On first packaging that
  folder is seeded once from `make run`'s save; after that the two keep separate saves.
- The launcher keeps the saves from before each of the last five sessions in `MK8D Data/backups`, and the previous
  session's log (`previous-run.log`, `crash-<time>.log`).
- Create an empty file `MK8D Data/windowed` to play in a window.
- **First launch is slow, once.** After each packaging, macOS checks the newly signed app (about a minute and a half
  before anything shows), the app imports the shader seed into its cache, and Metal rebuilds the pipelines the data folder
  has recorded before the title (43 s for about 8,000 on an M5, against 5 s on the next start). Later launches reach the
  title in seconds.

**The shader seed** (step 8 of `build_mk8d.py`; `--skip-seed` leaves it out). Every shader the game has is found in your
game files, translated to Metal and compiled ahead of time, instead of the first time a scene shows it in play. On an M5
the first build spends about 3 minutes translating and 15 compiling (14,976 Metal libraries); later builds reuse both.
The work folder holds 0.9 GB of seed and 0.2 GB of libraries; the app's copies are APFS clones. `assets/shader_list.tsv`
adds the bind states met in play that the default translations do not serve (shader keys and bind states only).
At start the app warms only the shaders this Mac has used (`TOTK_WARM_SCOPE=observed`; `all` warms the whole cache), and
the rest load from their precompiled libraries when first drawn. What a seed does not remove is building each render
pipeline the first time: the first race in a new data folder still builds a few hundred at its loading screen.

`--game` and `--update` take NSP or NSZ files. `--firmware` is your Switch firmware as a folder of NCA files: the system fonts
come from it. `--work` must be outside the checkout and outside iCloud Drive (Desktop and Documents are synced when that is on), with no space in its path; it holds data from your own game, so never
share or commit it. Every input is checked against [assets/expected-4.0.1.json](assets/expected-4.0.1.json): another version
stops the build. The build fetches SPIRV-Cross at a pinned commit into the work folder once, as Odyssey's does.

Tests (no game files): `python -m unittest discover -s games/mk8d/tests` from the repository root, with Xcode selected.

## Known limits

- **No Mii heads.** The game builds them from the console's MiiModel system data, which is firmware and not ours to ship. The
  host mounts an empty stand-in, so bodies and favourite colours show and heads draw nothing.
- No online play and no local wireless: the host answers offline, and the game takes its offline paths.
- The Booster Course Pass is not loaded yet (its add-on content is not mounted).
- The first visit to a scene still builds its render pipelines (the seed covers shaders, not pipelines); a played data
  folder warms the ones it has recorded.
- Shader list contributions are not collected for this game yet (the launcher sets no contribution file).
- The app contains game data from your own dump: never share it.

## Credits

[nxnative](https://github.com/seedds/nxnative) (MIT) ran this game natively first; its notes showed what the host needs (Mii
database, MiiModel stand-in, separate saves). This port imports no code from nxnative: each of those was written in this
runtime's own way, and the code comments say where an idea came from. The runtime it inherits from `games/odyssey` keeps
Odyssey's nxnative MIT notice (`third_party/licenses/nxnative.txt`). The runtime and its third-party code are described in
[third_party/COPIES.md](third_party/COPIES.md).
