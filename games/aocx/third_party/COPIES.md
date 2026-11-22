# Where this folder's code comes from

Hyrule Warriors: Age of Calamity runs on the Zelda-line runtime: the rewriter, loader, Horizon host and the NVN-on-Metal
graphics layer that Tears of the Kingdom, Animal Crossing, Super Mario Odyssey, Pokémon Legends: Arceus, Mario Kart 8
Deluxe and Red Dead Redemption use. It does **not** use nxnative code (the maintainer, 2026-11-21: "我们的项目不用 nxnative
代码，统一用塞尔达的代码，不维护两套"). `games/aoc` (this game on nxnative's runtime) and its notes were only read for
findings (versions, the system resource, the movie textures, the RDB / IDRK / GLSLC formats), never copied or built on.

The closest Zelda-line port is Red Dead Redemption (`games/rdr`, Mario Kart 8 Deluxe's runtime, the newest copy with the
offline console's answers, the load-order symbol binding and its other fixes); Mario Kart 8 Deluxe (`games/mk8dx`) has the
same three modules (main, the SDK's movie library subsdk0, sdk) and its movie path. Everything was taken from the working
tree of 2026-11-22 (after 4a194558) and then changed for this game:

| Here | From | Changed here |
|---|---|---|
| `runtime/` symlinks | `games/rdr/runtime` (the same links into `shared/runtime`) | `audio_out.c` linked to `shared/runtime/audio_out.c` (rdr's copy was the same file); `movie_host.c` and `movie_pace.h` linked to `shared/runtime` (Tears of the Kingdom's VP9 decoder: this game's movies are VP9 .webm) |
| `runtime/movie_host.h` | `games/totk/runtime/movie_host.h` (the header the shared `movie_host.c` builds with) | nothing |
| `runtime/` own files | `games/rdr/runtime` | `game_version.h` (AoC 1.4.1, `GAME_AOCX`), `game_info.plist` (bundle id); `loader.c` (SDK 9.6.0's four-argument `nn::init::Start`, as games/mk8dx); `services.c` and `runtime.h` (the movie decoder override back, as games/mk8dx; the system resource 0 as the NPDM says); `applet_host.c` (the screenshot copyright overlay, `nn::err::ShowError(ErrorCode)`); `input.c` (`GetNpadJoyHoldType`); `frontend.m` (its active players' id; a picture layer in no window for headless test runs) |
| `third_party/d2r/` (MIT) | `games/rdr/third_party/d2r` (its leading patch area; this game's modules keep the patch area after the image) | `patch_nso.py`: a byte jump table indexed through w30 (capstone names x30 `lr`; main+0x13847dc) |
| `third_party/d2r_gfx/` (MIT/BSD) | `games/rdr/third_party/d2r_gfx` (its links into `shared/` kept, the shared FPS panel `hud_panel.inc` included) | `hle_nvn.c`: `movie_host_tick` back, `nvnCommandBufferSetCopyImageStride` (games/totk's); `nvn_mtl.m`: the graphics and compute programs bound apart (games/totk's `S.prog` / `S.cprog` rule), a buffer-to-texture copy owns its texture (games/totk's `gpu_owned_mark`), the copy image stride, the concurrent-compilation switch for `GAME_AOCX`; `msl_loop_cap.h` linked from `shared/` (every translated MSL capped before it is compiled); a buffer texture in a slot of another type gets the dummy; the GPU hang guard (`TOTK_GPU_HANG_S`) and `mtl_quiesce`; `nvn_refl.{h,m}`: the c1 segment allocated (games/totk's form) and up to 64 KiB; `nvn_obj.h`: `p_copy_stride.image` |
| `third_party/smaa/`, `third_party/licenses/` | `games/rdr/third_party` | nothing |
| `prebuilt/macos-arm64/` | `games/rdr/prebuilt/macos-arm64` | `totk_xlate` links Tears of the Kingdom's current translator (`games/totk/prebuilt/macos-arm64`, with its SHA-256) instead of Odyssey's: combined texture handles get their slot's type (a73d65e2); the chain `aocx-native-v5-combined-handles` (`dmsh_chain.h`) |
| `scripts/prepare_runtime.py` | `games/rdr/scripts/prepare_runtime.py` | `assets/expected-1.4.1.json`; modules main, subsdk0, sdk |
| `scripts/merge_nsp_romfs.py` | new; imports `games/pla/scripts` (extract_exefs, extract_romfs, merge_romfs) and `games/rdr/scripts/merge_card_romfs.py` (`CardBase`, `romfs_section`) as they are | the base from the base NSP, the patch from the update NSP |
| `scripts/extract_dlc.py` | new, as `games/xc3/scripts/extract_aoc.py` does it; imports games/pla's and games/rdr's readers | this game's add-on titles, the files straight in `extracted/aoc/<index>` (Mario Kart's layout), its pins |
| `scripts/ktgl_shaders.py` | new (KTGL's RDB / IDRK / GLSLC containers; the formats read from games/aoc's notes, no code taken) | — |
| `scripts/prepare_aocx_shader_seed.py` | `games/rdr/scripts/prepare_rdr_shader_seed.py` (runs Odyssey's copy of the Zelda line's seed tool) | this game's discovery |
| `scripts/build_aocx.py` | `games/rdr/scripts/build_rdr.py` | this game's steps (two NSPs, the add-ons, three modules) |
| `Makefile` | `games/rdr/Makefile` | names, `GAME_AOCX`, libvpx (the VP9 movies) |
| `ios/` | `games/rdr/ios` | this game's profile (the add-ons in GameData, libvpx) and answers |
| `tests/` | `games/rdr/tests` (the leading patch area's tests and `synthetic_nso.py`) | this game's tests |
