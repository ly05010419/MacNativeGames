# Metroid Dread (work in progress)

Metroid Dread running natively the way `games/totk` runs Tears of the Kingdom. It is a fork of `games/by3` (itself
forked from `games/totk`) and keeps its own copies of everything in this folder (`runtime/`, `third_party/d2r`,
`third_party/d2r_gfx`, `prebuilt/`, `scripts/`), so changes here never affect another game and nothing here reads
another game's folder. **It is playable** on the Mac and on iPhone / iPad: the title, the save menu, the prologue and the
first area run with picture, sound, controller and saves. What was done and found: [CHANGELOG.md](CHANGELOG.md).

You must own the game: use your own dump and the `prod.keys` dumped from your own Switch. Nothing from the game is in
this repository; `Dread_build/` (the work folder) and `mods/` are ignored by git.

## Build and play

In one command from the top of the repository:

```sh
python3.14 games/dread/build.py "<game>.xci" -k prod.keys [-o OUT.app]
```

`games/dread/build.py` runs the four steps below in
`Dread_build/` next to the game file (reused by the next run), checks each against `assets/expected-v1.0.0.json`, and
writes `Metroid Dread.app` next to it. The app is a small launcher (window or full screen, monitor, window size,
MetalFX or plain scaling) that runs the game from the work folder: do not move or delete that folder. Saves:
`~/Library/Application Support/MacNativeGames/Dread/profile`; log: `~/Library/Logs/MacNativeGames/dread.log`.

### iPhone / iPad

```sh
python3.14 games/dread/build.py "<game>.xci" -k prod.keys --platform ios [-o OUT.ipa|OUT.app]
python3.14 games/dread/ios/tools/install_device.py "Metroid Dread.ipa" [--device NAME]
```

The same steps, then an unsigned app (`ios/tools/build_ipa.py`; needs the full Xcode and `brew install xcodegen cmake
ninja`) that carries the game (about 4.2 GB). `install_device.py` signs it with your Xcode team (a free Apple ID: 7 days)
and installs it over the cable; the first time, trust your Apple ID on the device (Settings > General > VPN & Device
Management). Play with a game controller or the on-screen controls (Switch layout; they hide while a controller is
connected and come back on a touch).

The device cannot translate shaders, so the build prepares them (`scripts/prepare_shader_seed.py`, a few seconds; the app
carries them in `GameData/shader-seed`). Every shader of the game sits in `system/shd/*.bshdat` (MercurySteam's MSHD
files: 1,010 unique shaders); each is translated for its default bind state, and for the other states
`assets/shader_list.tsv` names (cube maps and depth textures the defaults do not serve). The list holds shader keys and
bind states only; the build reads the shaders from your own RomFS. The app also carries what the Mac translated while
you played (`Dread_build/build/shader-cache`, with its pipelines). A draw needing a variant neither has is skipped, logged
as `[shader-miss]` and written to `shader-list-contribution.txt` in the app's Documents. To add the states you met:
`python3.14 games/dread/scripts/shader_list.py merge games/dread/assets/shader_list.tsv <that file or the Mac's shader
cache folder>`, then
`... shader_list.py prune games/dread/assets/shader_list.tsv Dread_build/build/shader-seed` keeps only the rows the
defaults do not serve. iOS also cannot run code written at run
time: the rewritten modules are compiled into the app's executable segments (`ios/tools/gen_embed.py`) and the loader
maps them back, the leading patch area included. Saves, the shader cache and `dread.log` are in the app's Documents
(the Files app shows them). `--simulator` (`build_ipa.py`) builds for the iOS Simulator but does not compile yet: the
Simulator SDK has no MetalFX.

## Inputs

- Base game `010093801237C000` 1.0.0 (v0), as the card image (XCI) or NSP. Updates are not supported yet.
- `assets/expected-v1.0.0.json` pins the Program NCA, every ExeFS file, the RomFS image and the rewritten modules.

## By hand

The same steps, from `games/dread`, with Python 3.14 and the packages of `requirements.txt`; the work folder is `../Dread_build`, next
to the card image in `games/`:

```sh
cd scripts
python3.14 extract_exefs.py "<game>.xci" --keys prod.keys --output ../../Dread_build/extracted/v100/exefs
python3.14 extract_romfs.py --keys prod.keys --manifest ../../Dread_build/extracted/v100/extraction-manifest.json \
  --output ../../Dread_build/extracted/v100/romfs
cd ..
for m in sdk subsdk0 main; do
  DM_RULESET=native DM_PRIVATE_GATES=0 python3.14 third_party/d2r/patch_nso.py \
    ../Dread_build/extracted/v100/exefs/$m ../Dread_build/rewritten/v100 $m
done
make prepare probe PY=python3.14
```

Checks of the rewrite (developer tools: LLVM for `structck.py`, `brew install llvm`, then
`LLVM_BIN=/opt/homebrew/opt/llvm/bin`; unicorn for `equiv.py`, `games/totk/requirements-dev.txt`):

```sh
python3.14 third_party/d2r/structck.py ../Dread_build/rewritten/v100 main ../Dread_build/extracted/v100/exefs/main
python3.14 third_party/d2r/equiv.py ../Dread_build/rewritten/v100 main ../Dread_build/extracted/v100/exefs/main
```

| Check | Result |
|---|---|
| ExeFS (main, sdk, subsdk0, rtld, main.npdm) | FS header, hash table and every block verified |
| RomFS | all six IVFC levels: 268,575 data blocks; 9,654 files, 4.4 GB; one flipped byte is caught |
| Rewrite structure (`structck.py`) | OK for all three modules, 0 violations |
| Rewrite semantics (`equiv.py`, emulator, random states) | main 1,085, sdk 11,975, subsdk0 10,486 sites, 0 divergent |
| Loader (`make probe`) | three modules mapped and sealed, 4,000 cross-module calls on 4 threads; a tampered image is refused |

## The leading patch area

`main` has a 1.8 GiB `.bss`. The rewriter normally puts its patch area after the whole image, which here is about
1.9 GB past the code, out of reach of a branch (±128 MiB). For such a module `patch_nso.py` puts the patch area
4 MiB before the module instead (`PATCH_LEAD` in `fixups.tsv`), and the loader maps it there. Smaller modules (sdk,
subsdk0 here, every module of the other games) keep the old layout and byte-identical output.

## What the game uses

- NVN (SDK 11.4.3): 524 names loaded, 523 in the graphics layer's table, 424 implemented (417 + 7 no-ops). Unimplemented
  ones abort with their name when called.
- NintendoWare Atk audio through `nn::audio`, Nintendo's movie library with WebM movies (the title screen is one),
  Lua 5.1 scripts, data in `packs/*.pkg`.

## Next

Longer play beyond the first area; frame rate and audio in the areas; the 2.1.0 update (a second set of hashes and a
rewrite of its code); shader libraries compiled on the Mac for iOS (`.metallib`: the device now compiles the seed's MSL
on first launch; needs Xcode's Metal Toolchain). Open findings are listed at the end of [CHANGELOG.md](CHANGELOG.md).
