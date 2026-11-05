# Switch 2 (NS2) in the Tears of the Kingdom runtime

NS2 dumps containing only ExeFS/RomFS have no Control-NCA icon. A game's `assets/app-icon.json` can identify its official
image by URL and SHA-256; preparation caches the verified image and generates `build/AppIcon.icns` outside the source
tree. Both the Mac app and the iOS asset catalog use that icon. Check the packaged `CFBundleIcons`, `Assets.car`, and
the installed phone's SpringBoard icon image; a successful build alone does not prove that a home-screen icon exists.

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
  TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1`). build_app.py does not know the game yet (Pokopia it does since 2026-11-04).

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

**Pokopia, later 2026-11-03: shaders prepared before the game runs; draws reach their programs.**
- `games/totk/scripts/ns2_shader_corpus.py --romfs extracted/pkp/romfs --modules extracted/pkp/elf/*.elf --out <corpus>`:
  every GLSLC output (0x00220015) in the modules, LINKDATA and the RDB data files, keyed as the runtime keys shaders:
  2,385 shaders in 90 s. `build/totk-native-pkp rewritten/pkp seed-shaders <corpus>` (TOTK_SHADER_CACHE, TOTK_RT_BG_WORKERS=8)
  translates each for the default bind state through totk_rt_variant: 2,379 in 20 s; the other 6 use Ampere 0x301, 0x883,
  0xf8c and a bindless LDC (open; their draws are skipped). `games/totk/scripts/build_shader_libs.py --cache <cache> --out
  build/shader-libs` compiles their MSL ahead (build_app.py's flags; the runtime loads them by the MSL's SHA-256): 1,645
  libraries in 59 s, none failing.
- Translator: F2FP.MERGE_C and F2I from an F16 half. A refused translation no longer ends a GAME_NS2 run or a seed: its
  draws are skipped (red line), TotK / BotW still stop.
- Replay: BindProgram binds the stages its mask names. A compute program no longer replaces the graphics one (Pokopia
  binds compute programs between draws: every draw had been skipped, a black picture), and separately bound vertex and
  fragment programs make a combined program (not yet checked in a run).
- Open: the frame loop sometimes stops at a varying frame (20 / 466 / 517); test windows float (`TOTK_WINDOW_FLOAT=1`, an
  accessory app, also over full-screen Spaces).

**Pokopia, afternoon 2026-11-03: first picture (the Ditto loading icon); a GPU hang at the first full scene is open.**
- Separable programs now combine (the log names them); `nvnTexturePoolRegisterImage` registers images in the texture
  table (compute images had named nothing: a GPU address fault, then a hang).
- Virtual cores move an owner to an idle core it may run on when a waiter can only use the owner's core (Horizon's
  migration of suggested threads): Pokopia's 0x1009 (core 1 only) starved behind 0x1023 with cores 2-5 idle, and the
  frame loop stopped at a varying frame. With it 13,000 frames in 4 minutes.
- Translator: BREV, ATOMS.POPC.INC (0xf8c), UP2UR (0x883) and the shared-memory index stride of LDS / STS / ATOMS
  (78..80, Ra shifted by stride + 1: the FFT compute shaders indexed shared memory wrongly). The seed translates 2,384 of
  2,385 (one bindless LDC left). Caches translated before this need translating again (the cache key does not change).
- Open: at the first full scene (frame ~640: 95 draws, 20 dispatches) the GPU hangs (kIOGPUCommandBufferCallbackErrorHang)
  and macOS ignores the process's later submissions (black). Not an indirect draw; the FFT and 70ab27e1 compute shaders
  are bounded. Diagnostics added: TOTK_SKIP_CS=all|<prefixes>, TOTK_SKIP_CS_LOG=1, TOTK_CS_UBO_LOG=<prefix>.

**Pokopia, evening 2026-11-03: the GPU hang is fixed; the game reaches its language selection.**
- Cause: a translator bug. A two-source half-pair operation (HADD2, HMUL2, HSET2, HSETP2, HMNMX2) in forms 2, 3 and 7 takes
  its second source from the 32-bit slot (here an immediate); patch 0006 took the unused register field at 64 instead (R0).
  Fragment shader 85446a67 (a bilateral blur) steps its loop with `HADD2 R6, R6, 2.0, 2.0`: the step became the bits of
  whatever R0 held, the loop never ended, the GPU hung, and macOS reset the GPU for every app (other apps' renderers failed
  too). Fixed as seedds/nxnative 03d92a9f (MIT) does: `AHalfSecond`, and an immediate's bits 62 / 63 are not modifiers.
  It changed 260 of Pokopia's 2,384 translations; Bananza's are Ampere too (its cache needs translating again).
- Found without hanging the GPU again: `DM_NVN_SKIP_FS=<prefixes>` (up to 16) halves the fragment shaders whose draws are
  skipped; `TOTK_FS_UBO_DUMP=<folder>` (new) writes the uniform buffers a skipped draw would have read. A hang test that
  hangs resets the whole GPU: run such bisections only with the user's agreement.
- Also: a compute pipeline whose kernel does not take its declared thread group is built again for it (nvn_mtl.m cs_new;
  none of Pokopia's needed it).
- Open: Pokopia's offline corpus misses some shaders the game draws. 85446a67 was translated at run time, and no 32-byte window
  of its instructions (but the common tail) is in the modules, LINKDATA or any IDRK entry, decompressed: it is not stored
  as it is drawn (made or changed at load, or in a container not read yet).

**Pokopia, night 2026-11-03: the language screen flickers black (about 10 frames in 12); fixed (see the next entry).**
- The chain behind it is the auto-exposure: compute 8699f064 clears a 64-bin histogram (storage buffer, 0x100 bytes),
  2118ab53 counts the scene's (texture ser 8190, read over 1728x972) log-luminance into it (threadgroup bins, then one
  global atomic add per bin: MATCH.ALL fails, so every lane adds its own bin), d0c17f1f reads the histogram back as a
  BUFFER texture (64x1, format 0xb, the same memory) and writes a 2x1 image, b2d97f59 copies it into 2x1 ser 5510, and
  2c74cdd9 writes 8 bytes the CPU reads. Uniforms of 2118ab53 c2[2] = (1/32, 0.5, 1728, 972): bins cover 2^-16..2^16.
- Fixed on the way (translator, Program.cs TexSlot): combined texture handles (nvnCommandBufferBindTexture, c[2] + 0x20,
  32 slots with the separate table's numbers) had no slot, so the runtime's per-slot target answer was never read and a
  fetch through one was typed as the instruction says. d0c17f1f's histogram fetch became a texture1d read of a texture
  buffer (undefined on Metal); now it is a texture_buffer read. 154 of Pokopia's run-cache variants named a combined
  handle without a type (moved to run2/cache-pre-combined and translated again). It did not end the flicker (1 of 12).
  Not yet compared over a Tears of the Kingdom cache: only fetches that ask the slot's type change (TLD / TLDS buffer
  test, TXQ, bindless), on combined handles.
- Measured with the bind trace (below): the histogram buffer reads all zero on the CPU in every frame from 3900 on, and
  2c74cdd9's result is [0, 1e6] (float 0 and 1,000,000). So 2118ab53 counts nothing: no texel of ser 8190 has a luminance
  above 0 in the clamp's window (or the reads miss). Next: dump ser 8190 and check its content and its writer (texdump of
  frame 4011 is in ns2-work/totk/run2/td_a), then why its luminance is 0 (pre-exposure from the previous exposure would
  close a loop: exposure 0 -> black scene -> empty histogram).
- Ruled out (A/B, 12 screenshots a second apart): present timing, TOTK_GPU_BLOCKRT, writeback, render stream, async
  replay, CPU deswizzle, window texture uploads, sync lag; ResourceAlias visibility on the barriers between dispatches
  (2 of 12).
- Diagnostics (default off): TOTK_BIND_TRACE=<key prefixes> ("," = every shader; TOTK_BIND_TRACE_FROM=<frame>,
  TOTK_BIND_TRACE_N=<count>): per encode, the textures / images / typed buffers (id, serial, memory, size, target, format,
  first floats), the storage buffers (up to 64 words) and the uniform buffers (12 floats) a shader reads; with it also
  [pool] lines (texture / sampler pool initialisation, bound pool changes, registrations and highest ID per pool).
  TOTK_QUEUE_TRACE, TOTK_T2T_LOG, TOTK_REUP_LOG, TOTK_INV_LOG, TOTK_PRESENT_FINISH, TOTK_DESWZ_CPU. A zero-copy linear
  texture tried on the way (2x1 exposure textures read from their pool) did not change anything and is not kept: those
  textures are written by the CPU.

**Pokopia, night 2026-11-03: the black frames fixed: a texture ID is checked against the bound texture pool.**
- The exposure chain was a lead, not the cause: the final picture is the background layer (ser 5431) copied into the
  window texture with the interface drawn over it, and in a black frame every render target was 0, the interface too.
  Screen captures of the window (not only the game's own shots) showed the same 9 black of 12.
- Pokopia makes three texture pools (33,024, 16,640 and 4,352 descriptors) and three sampler pools, registers its
  textures in the first, about 143 new IDs a frame, wrapping from 32,992 to 256 every ~229 frames. The ID bound check
  (an ID past the pool reads 0, as on hardware) took the capacity of the pool initialised last (4,352) and the replay
  ignored nvnCommandBufferSetTexturePool: every texture from ID 4,352 on read as an empty slot. Only the ~30 frames after
  each wrap had a picture. The log had said so all along ("池外贴图号 4352（池容量 4352）").
- Now the replay keeps the bound pool's descriptor count in the draw state (OP_SET_TEX_POOL -> S.tex_pool_n) and the
  check uses it. Texture IDs stay in one table for all pools, equal to hardware while one pool holds the registrations;
  a red line marks the first registration in a second pool (the sign the table has to become per pool). Pokopia: 12 of
  12 frames with a picture, past the language screen into its opening scene and the first area; Tears of the Kingdom
  1.4.3 boots to its title (155 s) as before.

**Pokopia, late 2026-11-03: the stalls in the world fixed: the control-memory callback is called.**
- In the world the game stopped for 0.4-0.6 s every 1.5 s, worse the longer it ran. Time Profiler: the replay thread
  spent the stalls in pin_scan; the log: 100,000 live recording copies (2 GiB) after four minutes, released none.
- Pokopia gives each command buffer its control memory once (1 MiB, 3 MiB) and adds more only from its memory callback
  (main+0x1144990, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_CONTROL_MEMORY). The host never called it: past the memory a
  recording carried no mark, the game never added memory again, and no copy was ever released.
- Now a recording that needs control memory beyond what is left calls the game's callback first (ctl_ensure; not for
  Breath of the Wild, whose handles are not in control memory), and the game adds memory: re-adding releases the copies
  recorded there (release_copies). Marks are used with the translation thread too: copy_free already keeps a released
  copy until every submission before it has been replayed; the sweep frees those waiting copies (retired_sweep).
- Same save, five minutes: 29,000 live copies / 7.8 MiB (was 120,000 / 2.5 GiB), frames over 200 ms 4 (2.2 s; was 103,
  59.5 s), 14,618 frames (was 11,722). Bananza 3 minutes as before (310 copies released by their marks), Tears of the
  Kingdom 1.4.3 to its title (2,500 live copies, as before). TOTK_CTL_LOG=1: callbacks, control memory additions.
- Still open for Pokopia: shaders compiled the first time a scene is entered (seconds, cached after); static
  block-compressed textures re-uploaded every submission as written (~170 MB a frame, TOTK_REUP_LOG); a second texture
  pool registering in some scenes (the table per pool); nvnCommandBufferDrawArraysIndirect and SetCopyImageStride.

**Pokopia, 2026-11-04: packaged as a Mac app by build_app.py, its shaders prepared at packaging.**
- `build_app.py <dump folder> --shared-fonts <folder>`: a folder as the game argument is a Switch 2 dump (base/,
  updates/<version>/ with exefs/ and romfs/); the title ID in its main.npdm picks the game (NS2 table: Pokopia
  0400A3A01EE4C000, update 655360), no keys. Steps: import_dump.py, the ExeFS / RomFS / rewrite checked against
  games/pokopia/assets/expected-v2.0.1.json (the verified rewrite), `make runtime-pkp` (launcher with GAME_POKOPIA /
  GAME_NS2), the shader seed, the startup libraries, the pipeline archive, the app (data folder Pokopia, bundle
  org.macnativegames.pokopia). The shared fonts are a TOTK / BotW build's (a dump has none). Mac only.
- Seed (scripts/prepare_ns2_seed.py): the corpus (ns2_shader_corpus.py), then the game binary's `seed-shaders
  <corpus> <shared list>` translates every shader for the default bind state and for each bind state the list names
  (totk_contrib_answers, so variants carry the list's names), into a cache kept in the work folder; the seed is that
  cache's translations in prepare_shader_seed.py's layout (seed-manifest.json, shader-keys.json), so the TOTK app steps
  take it as they are. Pokopia: 2,384 of 2,385 shaders, 21 s; 1,646 libraries.
- Shared list games/pokopia/assets/shader_list.tsv (identifiers only): merged from development runs. `shader_list.py
  merge` now renames a pso_user.tsv's on-the-spot variant names to the names a build gives (through the merged cache's
  .variants, as the runtime does for contributions): 636 of 640 rows had been refused. The dkb / pokopia lists carry A
  rows like TOTK's (OUR_RUNTIME).
- Measured (the in-world save, an empty shader cache, 4 minutes): frames over 100 ms 29 / 16.9 s without the prepared
  data, 29 / 14.2 s with the app; pipelines built in play 184 -> 90, libraries 190 -> 107; the title waited 3 ms. Every
  shader translated in play (99) is one the game files do not hold: Pokopia makes some shaders itself while it runs
  (xFractals/pokopia-native: 154 on 2.0.1, about 20 on the title screen; it records them and rebuilds). They are
  translated on first use and cached by the player; open: start their translation when the game finishes writing them
  instead of at the first draw. 337 warm rows of the list name such shaders and are left out of the app's lists.
- Translating them early (2026-11-04, measured, off by default): TOTK_PROG_TIMING=1 logs each stage's key at
  nvnProgramSetShaders and at the program's first draw. All 260 stages drawn in a 4-minute run had the key they had at
  SetShaders (the code is final there), 150 ms to 20 s before their first draw; but the game sets 8,179 programs (8,026
  shaders) in that time and draws 260. TOTK_SHADER_PREFETCH=2 translates every shader set without a cached
  translation in the background and compiles its library: built in play 90 -> 21 pipelines, 107 -> 27 libraries, frames
  over 100 ms 14.2 -> 10.9 s, but 6,207 translations and 2,456 libraries (about 930 s of thread time) at the first start,
  and a 5.1 s stop of the game's own threads at the start (the CPU taken). Next: the same on two background threads of
  low priority.
- Found on the way: a signed program written over in place (cp onto a file that had run) is killed at launch (SIGKILL,
  Code Signature Invalid): the seed tool replaces the translation service with a new file. GAME_NS2 reads
  build/game-data/<v>/process.txt, now in the app's game data.

**Pokopia, night 2026-11-04: all of its shaders found; one ID space per descriptor pool; two more NVN2 calls.**
- The shader extractor missed 98 % of the game's shaders. scripts/ns2_shader_corpus.py found the control section from
  other header fields and took it as 0xe0 bytes long; the GLSLC header gives the control section's offset (+0x60) and
  the control section its own size (+0x30) and the code length (+0x44), as kirakira-dev/ouncenative's
  nx2_shader_extract.py (MIT) reads them. Pokopia's files hold 131,090 distinct shaders (RDB data 130,904, LINKDATA 26,
  modules 160), not 2,385; of the 8,026 the game set in a 4-minute run, 7,717 are among them (the earlier note that it
  makes most of its shaders while it runs was wrong: about 65 drawn ones are in no file).
- The seed takes the modules' and LINKDATA's shaders and those of the RDB data the shared list names (ns2_shader_corpus.py
  --listed): all 131,090 would be about 23 GB of translations and libraries, and most are never loaded. The list holds the
  keys the game was seen to set (8,556 shaders, identifiers only); the runtime adds, to the player's contribution file,
  each shader the game sets that the seed lacks (totk_rt_note_set, GAME_NS2), so the list grows with play. shader_list.py
  prune refuses a Switch 2 list (its V rows are what the seed translates).
- One ID space per descriptor pool (seedds/nxnative's design, MIT): each texture / sampler pool gets a base in the host
  table at its first initialisation; registrations go to base + ID, the handles a command buffer binds get the base of the
  pool it last bound, and the replay's bound check takes base + size of the bound pool. Pokopia's second texture pool
  registered textures over the first one's IDs in some scenes. Games with one pool of each kind keep base 0. Deferred
  binds of a non-first pool are named in red (not rebased yet; Pokopia uses none).
- nvnCommandBufferDrawArraysIndirect (the GPU reads {count, instances, first, base instance}) and
  nvnCommandBufferSetCopyImageStride (the bytes between the 2D images of a 3D / array copy), both recorded as nxnative does.
- Synchronous replay (the default since eec5983b) needs a replay section: Donkey Kong Bananza submits from two threads, and
  their replays overlapped ("提交重入") and a memory-pool write-back on one thread used the other's upload batch. Submits,
  presents and every operation that waited for the translation thread (x_drain: pool / program finalize, Read / Write
  Texels, QueueFinish, suspend, memory-pool write-back) now hold a recursive lock with synchronous replay, and the thread
  holding it is the upload batch's owner; with the translation thread the section is x_drain as before.
- Measured (the in-world save, an empty shader cache, 4 minutes, the packaged app): frames over 100 ms 5.9 s (16.9 s
  without the prepared data, 14.2 s with the first app), of them while building shaders 3.2 s; pipelines built in play
  51, libraries 73. Bananza 3 minutes (11,700 frames) and Tears of the Kingdom 1.4.3 in the world (p50 9.6 ms) as before.
- Open: recording copies still grow slowly in Pokopia (56,000 live, 129 MiB after 6 minutes); DrawArraysIndirect was not
  called in the runs (no longer red); about 65 drawn shaders are made at run time (TOTK_SHADER_PREFETCH=2, to throttle);
  the import step of build_app.py's Switch 2 path was not run end to end (the test work folder had the verified import).

**Bananza regression found and fixed (2026-11-03 afternoon).** The vibration graph classes stubbed for Pokopia
(VibrationNode, VibrationTarget, VibrationPlayer, VibrationNodeConnection, VibrationConverter) emptied their constructors:
Bananza's own nodes derived from VibrationNode had no vtable and it called garbage from VibrationNode::Update about 20 s
in (main+0x2ee06c, signal 10). The SDK's own classes run again; Bananza runs 23,000 frames. Bisected away on the way:
the graphics scale and streamplay memory (back to the values Bananza was played with), core migration
(`TOTK_CORE_MIGRATE=0` turns it off), the fence-sync record rule (back to clearing at the record).
Missing floors in later areas: those draws use a vertex / geometry / fragment program, and the geometry stage has no
translation chain yet, so they are skipped (`TOTK_SKIP_LOG=1` names them). Next: geometry stages as compute (Ryujinx
`Translate(asCompute)`, as seedds/nxnative's nvn_geometry.inc does).

**Geometry stages as compute: Bananza's floors (2026-11-03 evening).** Done as the to-do below says (host
`nvn_geometry_rt.inc`, translator patch 0007; scheme and ABI from seedds/nxnative). Bananza has one geometry kernel,
5d537f76 (the terrain: triangles in, pass-through, 3 vertices out; it also appends through an atomic counter in storage
buffer s0 and a compare-and-swap table in s1), about 300 such draws a frame. Measured on the Mac, same save, same input:
- one draw at a time (`TOTK_GEO_RUN=0`, for comparison): correct, about 89 ms a frame;
- batched (default): correct, about 23-24 ms a frame (GPU about 20 ms, submit and present about 10-11 ms). Runs of
  58-64 geometry draws; none ended by a memory dependency, a few by an ordinary draw.

The batched path committed in 0cc0f18b drew wrong pictures (big black triangles over the scene; the commit message
called it correct). Causes, fixed at their source:
- the reader's input (the geometry output vertices, `g_geo_bind`) is not part of the bind state `S`, so `encode_args`
  reused the first geometry draw's argument block for the run's later draws in the same pass: their indices read the
  first draw's vertices. One draw at a time hid it (every geometry draw opened a pass of its own). The stage is now
  marked dirty around those bindings (`args_dirty_stage`).
- the look-ahead let ordinary draws and other commands pass. It now follows nxnative's `geo_run_walk`: only state
  changes between a run's draws (an ordinary draw, a render condition, a barrier, a copy, a dispatch end the run), and a
  draw joins only when it shares no written memory with the run's earlier jobs (vertex, uniform and storage buffer
  ranges; any other memory access counts as unknown and ends the run). Kernels are separated by device barriers.
- a geometry draw with rendering disabled ran its kernel (and its appends); the console skips the whole draw.
- `RED_ONCE` ids 141 and 160-171 wrote past its 128-entry array; it now has 256 and checks the id at compile time.

Frame-rate overlay: its resolution is the canvas the game presents, 2880x1620 for Bananza (the log line
`[overlay] resolution shown` names it when it changes). The internal resolution in brackets is found only for TotK's and
BotW's compose shaders (`scene_size_note`), so Bananza shows none: open. On a 3440x1440 display a 2880x1620 window
(`TOTK_WINDOW_SIZE`) is cut to 2455x1377 by macOS.

**Lean kernels for pass-through geometry shaders (2026-11-03 night).** As nxnative (dmsh_geometry.lean, geo_pack.metal
geo_lean_tri): the translator also writes `<variant>_l` for a pass-through shader with point, line or triangle input and one
invocation (tools/totk_xlate/GeometryLean.cs, named in the full kernel's layout as "lean"). It stores only each output
vertex's viewport mask and reads only the capture words it decides with (5d537f76: words 0, 1, 3, 16, 17, 18 of 144).
The host (`nvn_geometry_rt.inc`) captures those words, runs the lean kernel, turns each kept triangle into its original
vertex numbers (dropped ones collapse), and draws the game's own vertex and fragment shaders through them: no 134-word
output vertex, no reader. Taken for triangle input, one instance, and a vertex shader without memory writes;
`TOTK_GEO_LEAN=0` keeps the full kernel. Same build, same scene (the save had moved on, so this is not the scene above):
full kernel GPU 14.9 ms, frame 18.0 ms; lean GPU 12.0 ms, frame 16.5 ms (about 60 fps); the pictures match.

Then (same night): the run's kernels have no barriers between them (its jobs share no written memory, geo_run_conflict;
the barrier after every dispatch had been taken for the garbage triangles, whose cause was the argument block), and the
lean draws' host kernels run in phases with one barrier between phases (`nvn_geometry_lean.inc` geo_lean_phases). Same
scene: GPU 12.0 -> 11.0 ms; the frame stays at about 16.4 ms, the game's 60 fps. Point and line input take the lean path
too, packed: geo_lean_cull turns dropped vertices into restarts, nxnative's pack kernels (count, scan, write) compact the
strips into a list in primitive order with the count in indirect arguments, geo_lean_index maps them to vertex numbers,
and the draw is indexed indirect. Bananza has no point or line geometry draws; `TOTK_GEO_LEAN_PACK=1` sends its triangle
draws through the packed path as a test: same picture, GPU 11.3 ms.

Next for speed: mesh shaders (below).

**To do / to compare (geometry stages).**
- Done (above): geometry stages as compute (Ryujinx `Translate(asCompute)`: the vertex stage as a compute kernel writing its
  outputs to a buffer, the geometry stage as a compute kernel per primitive reading them, then the geometry output drawn
  with a pass-through vertex shader and the program's fragment shader). General, Ryujinx's own Metal backend's route,
  seedds/nxnative runs it; costs an extra dispatch or two and a buffer round trip per such draw.
- To compare later: Metal object + mesh shaders, vertex, geometry and rasterization in one pipeline with no intermediate
  buffer (in theory faster). Neither Ryujinx nor SPIRV-Cross generates mesh shaders, so it needs a translation backend of
  its own. Measure once the compute route is correct (Bananza's floors, Pokopia).

**To do: speed (paused 2026-11-03).** Bananza holds the game's 60 fps in the test scene (GPU about 11 ms, submit thread
about 6.9 ms); what was measured and planned:
- The primitive map on the GPU. Geometry costs the submit thread about 3.7 ms a frame: prepare 1.6 ms (reading the
  indices and making the primitive map 1.4 ms of it), the runs' walk and encoding about 1.9 ms, the rasters 0.2 ms.
  Bananza's terrain draws are triangle lists of 16-bit indices without restarts, lowest index 0, and the position stream's
  bound size is exactly their vertex count (816 bytes / 24 = 34 vertices, hi 33). So for a list without restarts the capture
  can take every vertex the vertex buffers hold and a GPU kernel can write record = index + base vertex from the index
  buffer itself (this also reads indices that GPU work of the same submission wrote). Written, built, not run or measured:
  the work folder's `patches/gpu-map-wip.patch` (applies to 4d9280f4 / 0201ee16; new file nvn_geometry_map.inc).
- The runs' encoding (about 1.9 ms a frame on the submit thread).
- Stutter: the slowest frame of each test run is 244-366 ms. Check that these are shader translations / library builds, then
  prepare Bananza's shaders offline as Pokopia's (scripts/ns2_shader_corpus.py reads Koei Tecmo's containers only; nxnative
  has split Bananza's).
- A repeatable scene: the work folder's run1/series.sh and quick.sh now restore run1/save-bench (the save of the scene
  above) before each run. Compare GPU time and submit time, not frame time, while the game holds 60 fps.
- Where the GPU time goes, per pass (games/totk/scripts/metal_perf_report.py, merged from main), before choosing the next
  GPU change; then mesh shaders (above).

## Offline shader depth mode

Offline preparation does not execute the game's `nvnDeviceSetDepthMode` call. Its depth mode must therefore be an
explicit build input: Bananza uses `minus-w`, Pokopia uses `zero`, as observed in their device setup. `build_app.py`
passes this to `prepare_ns2_seed.py --depth-mode`; direct `seed-shaders` invocations must set `TOTK_SEED_DEPTH_MODE`.
For `minus-w`, vertex translation converts clip z to `(z+w)/2` for Metal. Applying the wrong convention can clip the
entire fullscreen copy and leave the swapchain's previous contents on screen even with no missing shaders.

The seed manifest records `nvn_depth_mode` and `translation_context_sha256` (runtime binary, translator and requested
mode). Both offline translations and the app's writable shader cache use that context; changing it cannot reuse stale
vertex translations under unchanged shader keys. Offline pipeline generation reads the mode from the same manifest.

Verification uses one real fullscreen vertex shader translated in both modes: only `minus-w` must contain the clip-z
conversion. On the device, capture the presented swapchain texture, the final draw's actual uniform/vertex data and
consecutive frames, then check the visible cropped output as well as the window screenshot. A draw submission count
alone does not prove that the rasterizer produced pixels. Use a save copy and verify the restored original's hashes.

**Bananza custom rendering size (2026-11-04, Mac verified; iOS default 960x540 requested).**
`TOTK_OUTPUT_RES=1280x720` (or e.g. `960x540`) selects the initial native render size. The v458752 framebuffer maximum
getters at `main+0x5bf770/+0x5bf780` read an independent writable parameter page. Their original instructions are
verified on the Mac and exported into the signed iOS code; the phone only verifies the code and fills the data.
The verified `main+0x5399ec` copy publishes gsys's creation parameters. It skips the argument's 8-byte vtable:
its 0x108-byte payload has flags, 24 extents at +4, six output scales at +0xc4 and a 20-byte tail. Only the extents
and scales change. `ns2_resolution.h` defines and checks that layout; regression checks preserve the flags and tail
and reject malformed, overflowing and mismatched inputs. No NVN texture-size matching or mip remapping is involved.
Mac tests from save copies ran 1280x720 and 960x540 for about 70 s each, reached the mining tunnel and captured
canvas PNGs, crops and main render targets at exactly the requested size. The default-size gsys constant alone
is overwritten later and is insufficient. The iOS start screen offers 540p, 720p, 1080p and custom width x height;
Settings has the same engine parameter, default 960x540 (user request). Original texture dimensions and full mip chains stay intact.

The earlier iOS read-load Jetsam was VM page shortage near 4 GiB. A shared scratch-file heap mapping now defaults
on for Bananza on iOS only (`TOTK_FILE_HEAP=0` for comparison); macOS and other games retain their earlier backing.
The anonymous address/heap budget stays unchanged. The file is unlinked while mapped, so exit releases its disk space.
On the iPhone 14, 512 MiB CPU write-back/page-in and Metal physical/virtual alias checks passed, followed by a 263 s
original-resolution save-copy run that reached the tunnel without termination (peak footprint 1175 MiB; original
save restored and all hashes matched). Loading and pipeline creation were slow; this is not a claim of sustained
playable frame rate. The initial combined 720p/default-memory build was not installed because the phone disconnected.
The requested 960x540 build was subsequently signature-verified and installed over USB; all seven original profile
files matched their pre-install SHA-256 hashes. The first launch for verification was denied by iOS lock state,
so this build has not yet been verified in the real iPhone game. The test profile was removed and the original
profile restored with matching hashes. Evidence: external `codex/bananza-ios-20261104/{crash-after-save,resolution}/`.


**Bananza Mac display settings and 4K verification (2026-11-04).** Mac now defaults to native 1920x1080
when TOTK_OUTPUT_RES is absent. The Mac settings panel offers 540p, 720p, 1080p, 1440p, 4K and custom dimensions;
it stores graphics-output-size.txt beside controls.txt. The initial implementation applied changes on the next launch;
the live native rebuild described below supersedes that behavior. An explicit environment value sets the initial size.

4K needs the complete native memory chain. The graphics creation argument copied at main+0x53a068 has its
200 MiB primary dynamic render-pool budget at copied payload +0x20; the adjacent +0x28 is a 32 MiB fallback
block size, and the fallback heap is null in the real failure. The first attempted fallback-size change did
not affect the primary pool and was removed. The primary pool is now scaled by requested pixels above 1080p,
rounded to 64 KiB (4K: 800 MiB). Its parent GSys heap, from
System/HeapSize/Product.Nin_Ounce_NVN2.HeapDef.byml.zs, starts at 518144 KiB. Grow it by exactly the same delta
(4K: +614400 KiB), and add that delta to the reported application budget so other heap budgets stay intact.
Growing the primary pool alone cannot fit it into the original parent heap. The shared output-overlay builder
supports --version dkb, validates the original values and changes only GSys's Size field. The normal NS2 build
prepares 2560x1440 and 3840x2160; a custom size above 1080p needs its matching generated overlay.

Final real Mac tests used save copies and no TOTK_OUTPUT_RES override: default 1080p ran about 70 s, and a saved
4K setting ran about 121 s. Both reached the mining tunnel, captured native canvas PNGs at the requested size,
and exited normally. The 4K run's later footprint was about 5171 MiB. The setting was also selected in the actual
Mac menu and its saved file checked. Original texture dimensions, mip chains and effects were retained. These
are short load/image checks, not long gameplay or frame-rate acceptance. Evidence: external
codex/bananza-ios-20261104/resolution/mac-display-verification.json and mac-final-display-* directories.
The desktop command and installed 960x540 iPhone build have not been replaced by these Mac tests.


**Desktop entry installed (2026-11-04).** The existing `大金刚 Bananza.command` now starts the verified native
binary in the external `codex/bananza-ios-20261104/work/totk` tree. The player's original `play/profile` and
`run1/cache` paths are retained. Settings persist in `Application Support/MacNativeGames/Bananza` (a link to the
external artifact volume), including `graphics-output-size.txt`; keyboard 1 opens the panel. Default output stays
1080p until changed. The previous desktop entry is backed up beside `resolution/desktop-entry-record.json`,
which binds the entry to the exact executable tested and records the unchanged save hashes. Creating the entry
did not start the player's game. A per-launch command file lets the existing idle watchdog request a normal close.
The latest shared sources also compile for generic iOS. The installed phone remains the previous 960x540 build;
its exact-size real-game verification is still blocked by the phone's lock state. Full Mac-settings parity on iOS
has been requested and remains unimplemented.

Pre-commit validation also ran 1452 repository tests (7 skipped). The first run found one stale assertion counting
two device-translator build routes, updated for NS2's third route, and three synthetic update-helper scripts whose
shebang could not name a venv interpreter path containing spaces. The failing cases plus the complete device-translator
group passed a targeted 9-test recheck with a space-free path for those stdlib-only child fixtures. The complete suite
was not repeated after this targeted remediation. No game data, saves or raw captures are included in the commit.

**Live NS2 resolution and camera aspect (2026-11-04, Mac checkpoint).** Bananza and Pokopia settings now queue
native display changes during the current game. The resolution row, custom dimensions and 16:9 / 16:10 / 21:9 /
32:9 choices share that path and save the resulting dimensions. Windowed content follows the chosen size, scaled
proportionally to the available screen; fullscreen window bounds stay unchanged. Rounded-pixel capacity checks keep
32:9 at exactly 3840x1080 instead of losing a row to float rounding. Mac defaults remain 1920x1080.

Bananza's native display object belongs to SystemTask+0xd8. Its display calculation at main+0x30f85c consumes one
packed request, updates its native mode extents and invalidates the cached mode. The original renderer then rebuilds
its targets. With the prepared 4K overlay, framebuffer capacity, the dynamic render pool, GSys parent and application
budget reserve 3840x2160 while rendering at the selected extent. Pokopia snapshots requests before main+0x22d598,
uses the native size getters, then invalidates the cached mode after the committed canvas changes at +0x22d648.
Its bounded Resize(width,height,maxWidth,maxHeight) reserves both color and depth capacity. Reserving color alone
failed when an FSR view retained old 1080p color while depth had shrunk to 720p: native RT-stack Push rejected the
mismatched extents. Its +0x2964 is stack depth, not a frame index; the fix preserves the native target lifecycle.

Scene-camera aspect is updated before native projection/frustum construction: Bananza's registered projection
transfer (+0x31f3bc), and Pokopia's active gameplay-camera/render-view transfer (+0x8cd884). These change camera-owned
source parameters and invoke native cache updates; they do not stretch the final image or rewrite GPU matrices.
Pokopia also updates the selected standard UI camera's logical canvas before its per-frame snapshot (+0x1f9bcc8).
The native setter rebuilds the camera and adapter together. Custom UI cameras bypass this standard-camera path.

Real Mac save-copy runs continuously switched 16:9, 21:9, 32:9 and 16:10 and returned to 16:9. Both scene projections
matched the requested ratios and exposed additional horizontal world view. Pokopia's clean UI run lasted 161 s,
exited 0 and kept B/X icons at 30x30 pixels in 21:9; their horizontal position moved into a centered safe area.
Both original profiles matched their pre-run hashes. Earlier live-resolution runs also covered 540p through 4K;
Pokopia fullscreen bounds remained 3440x1440 across render-size changes. These are short correctness checks, not
long-session or performance acceptance. Evidence is outside the repository in codex/bananza-live-resolution:
dkb-camera-7, pkp-camera-6, pkp-live-5 and pkp-ui-final.

**Still open:** Bananza HUD aspect. Its scene is wide, but the last controlled UI comparison measured a heart at
43x31 pixels in 16:9 versus 56x31 in 21:9. Updating an outer DrawInfo did not resolve the authored root projection.
The experimental HUD changes are excluded from this checkpoint and retained externally under pending-hud.
The most recent root-pane run had additional settings changes and is not accepted as a controlled comparison.
This checkpoint does not claim complete Bananza ultrawide UI support. It also does not add the full Mac settings
panel to iOS or claim new phone gameplay verification; the installed phone remains the earlier 960x540 build.

Checkpoint validation: both Mac runtimes built; the Bananza app built for generic iOS, and Pokopia's NS2 patch
translation unit compiled for arm64 iOS. No new phone installation or device gameplay test was performed. The six
resolution/aspect/memory regressions and 19 settings/iOS-packaging checks passed. Full discovery ran 1454 tests
in 291 s with seven skips and three errors in synthetic hactool fixtures: their shebang used the venv interpreter's
space-containing path, so the OS could not launch them. With a separate space-free venv entry using the same installed
dependencies, all 15 update tests passed in 2.7 s; no product or test-source workaround was applied. Full discovery
was not repeated after that environment-only recheck. Logs and source/executable hashes are in the external
commit-validation.json, commit-tests.log, commit-updates-recheck.log and commit-*-build.log files.
