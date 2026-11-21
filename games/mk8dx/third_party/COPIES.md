# Where this folder's code comes from

Mario Kart 8 Deluxe runs on the Zelda-line runtime: the rewriter, loader, Horizon host and the NVN-on-Metal graphics
layer that Tears of the Kingdom, Animal Crossing, Super Mario Odyssey and Pokémon Legends: Arceus use. It does **not**
use nxnative code (the maintainer, 2026-11-21: "我们的项目不用 nxnative 代码，统一用塞尔达的代码，不维护两套");
`games/mk8d` (an nxnative copy) was only read for its findings, never copied.

The closest Zelda-line port is Super Mario Odyssey (`games/odyssey`): the same Nintendo EPD sead / agl engine family,
an SDK with `main` / `subsdk0` (the movie library) / `sdk`, NVN 53. Everything was taken from the working tree of
2026-11-21 (after c6851e4e) and then changed for this game:

| Here | From | Changed here |
|---|---|---|
| `runtime/` symlinks | `games/odyssey/runtime` (the same links into `shared/runtime`) | nothing |
| `runtime/` own files | `games/odyssey/runtime` | `game_version.h` (MK8DX 4.0.1, `GAME_MK8DX`), `game_info.plist` (bundle id); `game_patches.c` written anew (no patches; Odyssey's key 3 / key 4 sites are that game's addresses); `odyssey_aspect.c` dropped; `fs_host.c` (device save as a save of its own, add-on content, system data); `services.c` (add-on content list, offline and Mii overrides); `audio_renderer.c` (REV4 has no render-info output); `frontend.m` (`SetLayerScalingMode`); `input.c` (touch screen, Joy-Con assignment); `movie_host.c` (SDK 4.4's three-argument `movie::Decoder` constructor, the held-pictures deadlock fix, `TOTK_MOVIE_LOG`) |
| `runtime/shared_font.c` | `games/acnh/runtime/shared_font.c` (Odyssey's plus `GetSharedFont`, which this game asks for) | the message's path |
| `runtime/offline_host.c` | link to `shared/runtime/offline_host.c` (moved there from `games/totk/runtime`, which links it too) | built for `GAME_MK8DX` as well |
| `runtime/mii_host.{c,h}`, `runtime/mii_model.c` | new (this game's) | — |
| `third_party/d2r/` (MIT) | `games/odyssey/third_party/d2r` (= by3's, eow's) | `SIGNED_TABLES` rows for this game's `subsdk0` (two byte-class dispatches, `scripts/find_signed_tables.py`) |
| `third_party/d2r_gfx/` (MIT/BSD) | `games/odyssey/third_party/d2r_gfx` (its links into `shared/` kept) | `-DGAME_MK8DX`: NVN 53 device values, default depth mode and queue acquire as Odyssey / BotW; rendering into a block-compressed cube and into the levels a block texture's own chain does not reach (`blk_aux` in `nvn_mtl.m`) |
| `third_party/smaa/`, `third_party/licenses/` | `games/odyssey/third_party` | nothing |
| `prebuilt/macos-arm64/` | `games/odyssey/prebuilt/macos-arm64` (`totk_xlate` and its SHA-256) | nothing |
| `scripts/prepare_runtime.py` | `games/odyssey/scripts/prepare_runtime.py` | this folder's `relr.py` and `assets/expected-4.0.1.json`; the RomFS checked against its own index |
| `scripts/extract_mii_model.py` | new; imports `games/totk/scripts` (extract_exefs, extract_romfs, extract_shared_fonts) as they are | — |
| `scripts/index_romfs_image.py`, `scripts/find_signed_tables.py` | new | — |
| `Makefile` | `games/odyssey/Makefile` | names, `GAME_MK8DX`, no headless target, `boot` |
| `ios/tools/build_ipa.py`, `ios/Sources/ios_front.m` | this game's profile for `games/totk/ios/tools/build_port_ipa.py` and its answers to the shared shell, as `games/odyssey/ios` | the layout, the Mii parts and add-on content in GameData, the pane |
| `scripts/prepare_mk8dx_shader_seed.py` | new; runs `games/odyssey/scripts/prepare_odyssey_shader_seed.py` (on its copy of Tears of the Kingdom's seed tool) as it is | every RomFS folder, members of unnamed SARC archives, rule variants from the Mac cache |
