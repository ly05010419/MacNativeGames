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
