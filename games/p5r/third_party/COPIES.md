# Where this folder's code comes from

Persona 5 Royal runs on the Zelda-line runtime: the rewriter, loader, Horizon host and NVN-on-Metal graphics layer of
Red Dead Redemption (`games/rdr`), copied on 2026-11-22 and given this game's identity (`runtime/game_version.h`,
`GAME_P5R`, `GAME_INBOX_ID "p5r"`). It does **not** use nxnative code; nxnative's notes on this game
(`dm/games/01005CA01580E000`) were only read for findings. On 2026-11-23 it took Age of Calamity's changes to the same
graphics layer (`games/aocx`: the GPU hang guard, the graceful headless close, the loop cap, compute shared memory,
SetCopyImageStride, c1 beyond 4 KB) and links Tears of the Kingdom's `nvn_dirty.c` / `.h` (the split ranges,
`NVN_DIRTY_SPLIT`), `shared/app/app_launcher.h` (`app/`) and the shared headers in `third_party/d2r_gfx/gfx/nvn`.

Red Dead Redemption's own record of where its code came from follows (this copy's origin).

Red Dead Redemption runs on the Zelda-line runtime: the rewriter, loader, Horizon host and the NVN-on-Metal graphics
layer that Tears of the Kingdom, Animal Crossing, Super Mario Odyssey, Pokémon Legends: Arceus and Mario Kart 8 Deluxe
use. It does **not** use nxnative code (the maintainer, 2026-11-21: "我们的项目不用 nxnative 代码，统一用塞尔达的代码，不维护两套");
nxnative's notes on this game (`dm/games/01007820196A6000/README.md`, `game.tsv`, read from a clone outside the
repository) were only read for findings.

The closest Zelda-line port is Mario Kart 8 Deluxe (`games/mk8dx`, itself Super Mario Odyssey's line, the newest copy of
it with the offline console's answers and its fixes). Red Dead Redemption's SDK (15.3.2) is newer and its engine is
Rockstar's RAGE, so the SDK-15 forms were taken from the Zelda-line ports that already answer them (Metroid Dread, Echoes
of Wisdom, Tears of the Kingdom). Everything was taken from the working tree of 2026-11-22 (after ca0bda96) and then
changed for this game:

| Here | From | Changed here |
|---|---|---|
| `runtime/` symlinks | `games/mk8dx/runtime` (the same links into `shared/runtime`) | nothing; `movie_pace.h` not linked (no movie decoder) |
| `runtime/offline_host.c` | link to `shared/runtime/offline_host.c` | built for `GAME_RDR` too (its guard in `shared/`) |
| `runtime/` own files | `games/mk8dx/runtime` | `game_version.h` (RDR 1.0.4, `GAME_RDR`), `game_info.plist` (bundle id), `game_patches.c` (no patches); Mii files and `movie_host.{c,h}` dropped (main decodes its own Bink movies and imports no `nn::movie`); `loader.c` (SDK 15's five-argument `nn::init::Start`, as games/dread / games/eow; main's leading patch area, games/dread's; `runtime_module_data_rw`, games/dread's; global symbols bound in load order, as Horizon's rtld); `runtime.h` (the module's `patch_lead` fields, games/dread's); `services.c` (no system resource, as the NPDM declares; a thread stack in a module's .bss, games/dread's rule; no Mii or movie overrides); `fs_host.c` (`SetAllocator`, `EnableDefaultGlobalFileDataCache`, `DeleteDirectory`, `GetDirectoryEntryCount`, `GetSaveDataSize`: games/totk's and games/dread's answers; the system-data stand-in removed); `frontend.m` (the sized `CreateLayer`, `SetLayerCrop`, `DestroyLayer`, `CloseDisplay`, `vi::Finalize`, as games/eow); `applet_host.c` (`GetDefaultDisplayResolution` and its event, `RestartProgram`) |
| `third_party/d2r/` (MIT) | `games/mk8dx/third_party/d2r` (= Odyssey's) | `patch_nso.py`: the leading patch area of `games/dread/third_party/d2r/patch_nso.py` (a 1.2 GiB .bss puts the patch area out of branch reach), with a 16 MiB lead for this main's 20 MB of text |
| `third_party/d2r_gfx/` (MIT/BSD) | `games/mk8dx/third_party/d2r_gfx` (its links into `shared/` kept, the shared FPS panel `hud_panel.inc` included) | `hle_nvn.c`: `nvnSamplerPoolRegisterSamplerBuilder` (NVN 55), no `movie_host_tick` |
| `third_party/smaa/`, `third_party/licenses/` | `games/mk8dx/third_party` | nothing |
| `prebuilt/macos-arm64/` | `games/mk8dx/prebuilt/macos-arm64` (the link to Odyssey's `totk_xlate` and its SHA-256) | nothing |
| `scripts/prepare_runtime.py` | `games/mk8dx/scripts/prepare_runtime.py` | `assets/expected-1.0.4.json`; modules main and sdk; the module's bytes start `PATCH_LEAD` into its file |
| `scripts/merge_card_romfs.py` | new; imports `games/pla/scripts` (extract_exefs, extract_romfs, merge_romfs) as they are | the base RomFS read from the card instead of an extracted 11 GB image |
| `scripts/build_rdr.py` | `games/mk8dx/scripts/build_mk8dx.py` | this game's steps (the card image; no Mii parts or fonts) |
| `Makefile` | `games/mk8dx/Makefile` | names, `GAME_RDR`, no libvpx |
| `tests/` | new | — |
