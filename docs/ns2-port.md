# Switch 2 (NS2) in the Tears of the Kingdom runtime

Branch `ns2` (started 2026-11-02). Goal: Switch 2 games run on the same runtime as Tears of the Kingdom and Breath of the
Wild (`games/totk/runtime`, `third_party/d2r_gfx`, the `totk_xlate` translator), not as a separate copy of another
project. First target: **Donkey Kong Bananza** (title `040074A01BF12000`, update v458752, SDK 20.5.19, NVN2) on the Mac,
because seedds/nxnative (MIT) already runs it there and every step can be compared with it.

## Reference

- nxnative at `e52afc48`, local clone `/Volumes/mac-APFS/工具/nxnative`. Its Switch 2 work, in order: `8dd0c98a` (boot into
  the frame loop, dump packing), `d3995ca4` (Ampere shaders translated natively, menu renders), `a7bd05f4` (tensor
  instructions for agl's upscaler, NVN2 compute c0 and push constants, mip chains), `fdb3710d` (Switch 2 GOB order),
  `5b2d4606` (`nn::codec` video decoder, sync point events), `5251763d` (jump tables, BRXU, REDUX), `c4b0618c` (plays:
  terrain through loaded pointers, Switch 2 device values), `4de40e62` (60 fps), `99947471` (dump folders without keys),
  `c8521eeb` (Splatoon Raiders), `163a9905` (NVN2 versions for SDK 21.4.4). Its game note:
  `dm/games/040074A01BF12000/README.md`.
- About 6,300 lines of code in those commits, besides generated tables: the Ryujinx translator about 3,900 (five new Ampere
  files and about fifteen changed ones), the NVN layer about 3,300 (2,600 of them the generated `nvn_thunks.S`, 202 NVN2
  names), the host about 600 (`hle_codec_video.c` 430), packing about 200.
- Our `vendor/ryujinx` is Ryujinx 1.3.2 unmodified; nxnative's copy before its Switch 2 work (`8dd0c98a^`) has 166 of the
  190 shader translator files identical to ours, the other 24 carry its own earlier fixes. Our translator changes stay in
  `games/totk/tools/totk_xlate/patches/*.patch`, applied by `publish_native.sh`.
- The game: the dump `/Volumes/工作/SW2-dumpster/DonkeyKongBananza` (`base/`, `updates/458752/`, `dlc/`). nxnative's
  build of it, with every translated shader for comparison: `/Volumes/mac-APFS/SW2-dumpster/040074A01BF12000_build`.

## Phases

Each phase ends with a check that needs no one at the keyboard, and is committed on `ns2` on its own.

| # | Phase | Done when |
|---|---|---|
| 1 | **Ampere shader translation**: nxnative's decoder and emitters (ALU, memory, tensor, attributes), the NVN2 control section (magic `0x98761234`, version `0xc9`, its lengths and register count), as patch `0006` on `vendor/ryujinx` and in `Service.cs` | Every Bananza shader in nxnative's split translates with our translator, and the MSL matches nxnative's (or each difference is explained) |
| 2 | **Packing a dump**: a dump folder (`base/`, `updates/<version>/`, `dlc/`) as input, no keys, a control record made up from the profile, the RomFS image; Switch 2 modules (`main`, `sdk`, `subsdk*`) through `patch_nso.py` | `build_app.py` prepares Bananza's modules and data; the rewriter's counts match an instruction census of the modules |
| 3 | **Host for SDK 20.5**: what nxnative's host answers for the Switch 2 SDK (GetInfo 0x21 for the VAMM granule, 9 GB application pool, the services and SDK calls of its `hle_*` changes) | The game reaches its frame loop on the Mac (log) |
| 4 | **NVN2**: the 202 new entry points, NVN2 shader keys, compute c0 and push constants, the Switch 2 GOB order in the deswizzle, sync point events | The menu renders |
| 5 | **The rest of the frame**: tensor instructions and agl's NDSR upscaler, mesh geometry, `nn::codec` video | The first level renders and plays with its guide movies |
| 6 | **Speed and the iPhone** | 60 fps on the Mac as nxnative; then the iPhone build |

## Rules for this work

- Port behaviour, not copies: each change is read in nxnative, written into our code where our code does the same thing,
  and credited (`from seedds/nxnative <commit>, MIT`) in the comment, as earlier ports from it.
- No Bananza-only branches where a Switch 2 rule holds for every Switch 2 game (NVN2, SDK 20 / 21): key them on what the
  module shows (its NVN2 control sections, its SDK version), the way the runtime already keys TotK and BotW behaviour.
- The TotK and BotW builds must not change: every phase that touches shared code is checked against them (the
  translator replay `scripts/diag/retranslate_cache.py --compare` for phase 1; their unit tests and a Mac boot for the rest).

## Status

**Phase 1, done 2026-11-02.** `totk_xlate/patches/0006-ampere-switch-2-shaders.patch` carries nxnative's Ryujinx changes
`8dd0c98a^..c8521eeb` (Ampere decoder and emitters, tensor pass, NVN2 compute c0, goto elimination without nested loops),
its image atomics at MSL 3.1 (`c3a08803`, `2e655b79`, `f8642c48`) and its texture gather with register offsets as four gathers
(`b3f25a09`); they apply to our Ryujinx 1.3.2 with patches 0001-0005 except one context line (`TranslationFlags.Ampere`,
added by hand at nxnative's value `1 << 4`). `totk_xlate`: job `arch: sm86` sets `TranslationFlags.Ampere`, `nvn2_cs` gives an
NVN2 compute shader's c[0] layout, a request's `shared` is passed through, and every job sets `MetalImageAtomics`;
SPIRV-Cross writes a module with `OpImageTexelPointer` at MSL 3.1 (`Service.cs MslVersion`), every other at 2.3 as before.
The prebuilt `totk_xlate` is rebuilt with it (`publish_native.sh`, .NET 10, no error).
Checks:
- Bananza (`check_bananza.py`, outside the repository: every record of nxnative's Bananza split sent to our service with
  `arch: sm86`, its c1 segment and, for compute, `nvn2_cs` and the thread group of nxnative's facts): **22,845 of 22,845**
  vertex, pixel and compute shaders translate; sampled MSL and both image-atomic compute shaders compile with Xcode 26.6.
  Open: 7 geometry shaders (our service has no geometry stage; nxnative draws them as mesh geometry, phase 5).
- TotK 1.4.3 (`scripts/diag/retranslate_cache.py --compare` over the 8,673 variants of a v4 cache, main's prebuilt translator
  against this one): 8,673 translate, 0 fail; 1,890 identical; 6,735 differ only in `precise::` for `fast::` (main's
  `0cd6cc1d`, now built); 48 differ in SPIR-V: the goto-elimination change leaves out a `do { } while (false)` around the
  program (48 loops in main's MSL, 0 here), and all 48 compile. The MSL changes under the same variant names, so the chain is
  `totk-native-v9-unverified` now. BotW shares the translator and the chain; its shaders are Maxwell as TotK's.

**Update 2026-11-03 night:** a 10-minute play session (walking, punching, jumping) ran 49,662 frames (about 83 fps) with no
crash; the cave and tunnel render clean after the mip-chain fix. TotK 1.4.3 alive and rendering at 155 s (Link in the cave).
Open besides the list below: a scaled texture-to-texture copy (2304x1296 -> 360x202, likely a menu background or thumbnail)
is not implemented (red line, skipped).

**Phases 2-4 and part of 5, 2026-11-03: Donkey Kong Bananza plays on the Mac** (M-series Mac, macOS 26.5, five-minute
scripted sessions): the mode menu, the opening text, the mining tunnel with Donkey Kong, rocks, rails and the HUD (gold
counter rising), the guide movie looping in its panel, about 50 fps; no crash in 300 s. TotK 1.4.3 boots and renders with
the same runtime (100 s, no failure); its tables are byte-identical to before.

How it is built and run (outside the repository; game files stay with the dump):
- `games/totk/scripts/import_dump.py <dump> --output extracted/dkb` (ExeFS + RomFS image, 77,342 files, 9.2 GB), then
  `prepare_modules.py`, the rewriter (`DM_RULESET=native DM_PRIVATE_GATES=0 patch_nso.py` for `sdk` and `main`),
  `verify_rewrites.py --version dkb` (all gates pass), `make game-data-dkb runtime-dkb SHADER_LIST=../dkb/assets/shader_list.tsv`.
- `build/totk-native-dkb rewritten/dkb boot` with the launcher's GPU settings (`TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1
  TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1`). build_app.py does not know the game yet.

Found on the way (general fixes, each in the code with its reason):
- a mip-chain pass sampled the level it renders (Metal fetches it at weight 0; undefined data, 0 x NaN): the environment
  cube's NaN spread as bright blotches with black specks; the draw now samples the levels below (nvn_mtl.m mip_feedback_view);
- the equivalence checker modelled an ARMv8.0 CPU (no LSE atomics, LDAPR) and counted alignment exceptions as differences;
- released recording copies were freed while the translation thread still had them queued (a game may add control memory
  again right after a submit; TotK never did): freed once every earlier submission is replayed;
- host replacements with more than 8 integer arguments read host-stack garbage: the stack switch now copies the guest's
  first 8 stack slots (nn::codec's NV12 readback, 12 arguments; it also brought the 3D scene);
- NVN_EXTRA implementations were hidden behind the table's stubs once the NVN2 names were added.

Open:
- geometry stage (7 shaders, mesh geometry in nxnative): their draws are skipped;
- storage through pointers loaded from memory is bound to an empty buffer (nxnative's memory pool table not ported):
  the terrain's indirect draw records are not reset by gsys's vertex shader;
- unimplemented NVN2 entries answer 0 with a red line (non-fatal in GAME_NS2 builds); 60 entries nxnative implements are
  missing here (transform feedback, indirect-count draws, ClearBuffer, ReportValue, CommandBufferFenceSync, ...);
- add-on content is not packed; H.264 / AV1 video not served; Switch 2 device values (nvnDeviceGetInteger) are Switch 1's;
- build_app.py packaging, an app bundle, iPhone; the BotW and TotK 1.0.0 binaries do not link on main either (VR symbols),
  unrelated to this branch.

**Pokemon Pokopia (`0400A3A01EE4C000`, update 655360 / display 2.0.1), 2026-11-03: boots and runs its frame loop, picture
black.** Variant `runtime-pkp` / `GAME_POKOPIA` (with `GAME_NS2`), profile folder `pkp`, `games/pokopia/assets`. It passes
nnMain, the graphics, audio and hid setup and runs past frame 1,200 with no failure; no game picture yet (draws without a
translated vertex stage are skipped: open). Reference: kirakira-dev/ouncenative (MIT; nxnative fork with NX2 support, Pokopia
v327680 with offline PokeDinner shaders, GPL patches not taken).
Added for it (general where possible, each with its reason in the code):
- rewriter: `mov sp, x18` (alloca) rewritten (`site_sp`), structck accepts a scratch register saved through an `entry_sp`
  alias;
- host: AudioOut (AudioQueue), Opus multistream decoders (libopus), fs (allocator, free space, save data size / extend,
  directory entry counts, uncached rom mounts), time (location, zone rules, network clock tasks), account / nsa / friends /
  ldn / prepo / bcat / ssl / socket stubs answering offline, hid single-state reads, hold type, handheld activation, the
  public six-axis state;
- translator: Ampere MEMBAR (group or device memory barrier), ERRBAR and CCTL (no-ops after it), ATOMS (shared atomics);
- NVN: fence syncs on command buffers; `CommandBufferFenceSync` no longer clears the sync at the record (the engine polled
  a sync of a command buffer not submitted yet forever; the frame loop stopped at ~517); stencil views (X32_Stencil8);
  recording modes 0-2 checked; `oe::GetCurrentGraphicsScale` 0 and streamplay work memory 0 as PokeDinner answers
  (Bananza not re-checked with these two);
- test scripts: `TOTK_WINDOW_FLOAT=1` keeps a test window above other windows (a covered window stops presents).
