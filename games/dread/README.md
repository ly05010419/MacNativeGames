# Metroid Dread (work in progress)

Metroid Dread running natively the way `games/totk` runs Tears of the Kingdom. It is a fork of `games/by3` (itself
forked from `games/totk`) and keeps its own copies of everything in this folder (`runtime/`, `third_party/d2r`,
`third_party/d2r_gfx`, `prebuilt/`, `scripts/`), so changes here never affect another game and nothing here reads
another game's folder. The target is **2.1.0**, the latest update. **On the Mac it is playable**: the title, the save
menu, the prologue and the first area run with picture, sound, controller and saves, and a scripted run of the app
reaches Artaria. For iPhone / iPad the build scripts are updated for 2.1.0, but no iOS package was built: 2.1.0 was
validated on macOS only (1.0.0 was played on a device).
What was done and found: [CHANGELOG.md](CHANGELOG.md).

You must own the game: use your own dump and the `prod.keys` dumped from your own Switch. Nothing from the game is in
this repository; `Dread_build/` (the work folder) and `mods/` are ignored by git.

## Build and play

Use MNG Launcher or its command-line engine:

```sh
python3 tools/mng.py build "<base>.xci" "<2.1.0 update>.nsp" --keys "<prod.keys>"
python3 tools/mng.py update dread
```

Leave out the update to build the still-supported 1.0.0 base. Each version uses its own pinned ExeFS, rewrite and
RomFS hashes; the Launcher accepts the 2.1.0 update NSP. The shared work-folder policy records `MNGWorkFolder`
in the App, so later express updates use that directory. An explicit `--work` must point outside the checkout.

The original external game-data layout is retained: keep the recorded work directory to play. The App now has a
separately signed `ShaderData-dread.bundle`, the exact warm-up lists beside its shader map and an offline pipeline
archive. Packaging builds descriptors in parallel outside the App and verifies the saved archive in a fresh process.
Launch waits for every bundled list and translated variant before the title. Rebuilds preserve the App directory;
express updates retain translations, usable descriptor entries, digest records and matching signed shader data.
Saves remain in `~/Library/Application Support/MacNativeGames/Dread/profile`.

The packaging integration was checked with synthetic Metal/native tests. A complete game build and gameplay
check for these new packaging paths is still needed; the earlier gameplay measurements below predate this integration.
Shader contributions remain local for manual merging with this game's tool; this backend's bind-state format has no
registered online inbox schema. It does not upload through another game's identity.

### iPhone / iPad

```sh
python3.14 games/dread/build.py "<base>.xci|nsp" --update "<2.1.0 update>.nsp" -k prod.keys --platform ios [-o OUT.ipa|OUT.app]
python3.14 games/dread/ios/tools/install_device.py "Metroid Dread.ipa" [--device NAME]
```

The same steps, then an unsigned app (`ios/tools/build_ipa.py`; needs the full Xcode; xcodegen, cmake and ninja
come with the build, pinned) that carries the game (about 4.2 GB). `install_device.py` signs it with your Xcode team (a free Apple ID: 7 days)
and installs it over the cable; the first time, trust your Apple ID on the device (Settings > General > VPN & Device
Management). Play with a game controller or the on-screen controls (Switch layout; they hide while a controller is
connected and come back on a touch). With 2.1.0 these steps are updated but not run: no iOS package was built yet.

### Shaders and pipelines (Mac)

The Mac app does not translate or compile shaders while you play the opening. The build prepares every shader of the
game from your RomFS:
- `make shader-seed`: 1,045 variants of 1,010 shaders, imported at the first start;
- `make shader-libs` (`scripts/build_shader_libs.py`): compiles them into Metal libraries (805 unique sources), loaded
  ready-made.

The seed's warm-up list also holds the 128 render pipelines the opening builds, from the `R` rows of
`assets/shader_list.tsv`. They are built in the background before play. On the route from the start to Artaria,
synchronous pipeline builds in play go from 128 to 0 (A/B, two runs per arm); both arms recorded 47 frames over 50 ms.
Pipelines of later areas are built the first time and kept in the profile's `pso_user.tsv` for the next start.

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

- Base game `010093801237C000` 1.0.0 (v0), as the card image (XCI) or NSP. The 2.1.0 builds were checked with an NSP
  base; a card image goes through the same container reader, untested here.
- Update `010093801237C800` 2.1.0 (v327680), as an NSP. Only `main` changes (build ID `646761F6…CEF93DC1`); `sdk` and
  `subsdk0` are byte-identical to 1.0.0's.
- `assets/expected-v2.1.0.json` pins both packages, every ExeFS file, the merged RomFS image and the rewritten
  modules.

## By hand

The same steps, from `games/dread`, with Python 3.14 and the packages of `requirements.txt`; the work folder is `../Dread_build`, next
to the card image in `games/`:

```sh
cd scripts
python3.14 extract_code.py "<update>.nsp" --keys prod.keys --title 010093801237C000 \
  --output ../../Dread_build/extracted/v210/exefs
python3.14 merge_romfs.py "<base>.xci|nsp" "<update>.nsp" --keys prod.keys --title 010093801237C000 \
  --output ../../Dread_build/extracted/v210/romfs
cd ..
for m in sdk subsdk0 main; do
  DM_RULESET=native DM_PRIVATE_GATES=0 python3.14 third_party/d2r/patch_nso.py \
    ../Dread_build/extracted/v210/exefs/$m ../Dread_build/rewritten/v210 $m
done
make prepare probe shader-seed shader-libs PY=python3.14
```

Checks of the rewrite (developer tools: `structck.py` disassembles with Xcode's `llvm-objdump`;
unicorn for `equiv.py`, `games/totk/requirements-dev.txt`):

```sh
python3.14 third_party/d2r/structck.py ../Dread_build/rewritten/v210 main ../Dread_build/extracted/v210/exefs/main
python3.14 third_party/d2r/equiv.py ../Dread_build/rewritten/v210 main ../Dread_build/extracted/v210/exefs/main
```

| Check | Result |
|---|---|
| ExeFS of the update (main, sdk, subsdk0, rtld, main.npdm) | FS header, hash table and every block verified |
| Merged RomFS (base + update) | all six IVFC levels: 323,482 data blocks; 9,774 files, 5.3 GB |
| Rewrite structure (`structck.py`) | OK for all three modules, 0 violations |
| Rewrite semantics (`equiv.py`, emulator, random states) | main 1,087, sdk 11,975, subsdk0 10,486 sites, 0 divergent |
| Loader (`make probe`) | three modules of `rewritten/v210` mapped and sealed, 4,000 cross-module calls on 4 threads |
| Unit tests (`make test`) | texel regions; the movie decoder's Stop / Reset with a frame held (buffer and texture mode) |

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
- Streamed music and movie audio as DSP-ADPCM in buffers of 14,336 samples at 44.1 kHz, every one marked end of stream
  with more queued. The renderer plays on into the next queued buffer in the same 5 ms frame, as the DSP does
  (`AUDIO_EOS_PLAYS_ON`, `runtime/audio_mixer.h`). Stopping for the rest of the frame was a tick every 325 ms.
- The movie decoder (`runtime/movie_host.c`) clears the frames a game still holds when it stops or resets a movie, so
  a late GetOutputBuffer answers ST_ERROR instead of reading a freed frame (Echoes of Wisdom's fix,
  `tests/movie_stop`).

## Next

Longer play beyond the first area; frame rate and audio in the areas; validating the iPhone / iPad build on 2.1.0;
shader libraries compiled on the Mac for iOS (`.metallib`: the device compiles the seed's MSL on first launch); the
pipelines of later areas in the warm-up list. Open findings, the earlier movie crashes among them, are listed at the
end of [CHANGELOG.md](CHANGELOG.md).
