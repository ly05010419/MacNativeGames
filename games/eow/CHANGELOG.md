# The Legend of Zelda: Echoes of Wisdom port: work log

A record of how this port is being made, for anyone who continues it. It follows `games/by3` (Bayonetta 3), whose
work log documents the same stages; this folder started as a copy of `games/by3`. Each entry says what was found, what
was done and why, what went wrong on the way, and how it was checked. Newest entries go at the bottom of [Log](#log);
[Status](#status) is kept current.

Conventions: dates are absolute; "verified" means a check with a recorded result, never "it looked right". Game data,
keys and saves are never committed (`games/eow/EoW_build`, a link to the work folder, and `games/eow/mods/` are in
`games/eow/.gitignore`).

## Status

| Stage | Goal | State |
|---|---|---|
| 0 | Feasibility: does the game fit the TotK architecture? | done 2026-10-24 |
| 1 | Code and data out of the dump; code rewritten for Apple silicon | done 2026-10-24 |
| 2 | A runtime target with the TotK / Bayonetta-only parts compiled out, booting to `nnMain` | done 2026-10-24 |
| 3 | Follow every unknown service / command / NVN call until the first `nvnQueuePresentTexture` | done 2026-10-24 (GPU submission off) |
| 4 | A picture, audio, input, saves | playable: title, prologue, sound, controllers, saves (2026-10-24); about 1.5 h of play logged by 2026-10-27 |

## Log

### 2026-10-24: stage 0, feasibility

Inputs: base `01008CF01BAAC000` v0 (NSP) and update 1.1.0 `01008CF01BAAC800` v196608 (NSP), `prod.keys` from the same
Switch (33 malformed `*kek_source*` lines with 34 hex digits; a filtered copy was used and deleted afterwards).

- **Modules**: `main`, `sdk`, `subsdk0`, `subsdk1`, `rtld`, `main.npdm`, all from the update (`scripts/extract_code.py
  --title 01008CF01BAAC000`). SDK 20.5.1 (TotK 1.4.3 is 20.5.22). main build ID
  `9F221E1AFA65BA5443E955158AB97DA871E5E41F`.
- **Graphics are NVN**, with the same 569-name table as TotK. Middleware: NintendoWare G3d / Ui2d / Vfx / Font / Atk
  20.5.1, PhysX 3.4.
- `subsdk0` is NVN's GLSLC shader compiler; main imports `glslcCompile` / `glslcCompileSpecialized`. `subsdk1` is the
  movie library (the size of TotK's `subsdk0`).
- main imports 144 `nn::` functions TotK's main does not (49 audio, 21 hid, 16 os, 16 util, 12 nfp, 8 lmem).

### 2026-10-24: stage 1, code and data

**The base is compressed, not sparse.** The pinned hactool stops with "Too big offset looked up in BKTR relocation
table", as with Bayonetta 3, but the cause differs. EoW's base RomFS section (`7f18d795…`, 5.8 GB stored) has sparse
generation 0. Its FS header carries **compression info** at 0x178: a bucket tree (`BKTR`, entries of 0x18 bytes:
virtual offset, physical offset, type, level, physical size). The update's RomFS section has one too, on top of its
indirect (patch) layer.

`scripts/merge_romfs.py` (from by3) now reads, bottom up:

1. base section: plain AES-CTR (the sparse layer is skipped when its generation is 0);
2. update section: AES-CTR-Ex, then the indirect table over (base section, update data);
3. IVFC over the merged *stored* image: all six levels verified there;
4. compression: the table is the **last part of the verified IVFC data level** (offset 0x1567c4000 + size 0x24c000 =
   level size 0x156a10000). The RomFS is the decompressed view of that level (`romfs.bin`); the stored level is
   written to `romfs.stored.bin`, verified, read through the table and deleted.

Three layout differences from Bayonetta 3 were found by reading the dump, not assumed:
- The update's AES-CTR-Ex table ends at the indirect table (Bayonetta 3's covers it); the indirect table itself is
  plain AES-CTR with the section IV. Either form is accepted; any other end offset stops the script.
- The base's compression table is padded to 154 nodes, more than its 98,846 entries need (146). The bucket-tree check
  now accepts a padded size (whole nodes). That table is never used: the indirect table maps the base's stored bytes.
- Stored (uncompressed) ranges carry physical size `0xffffffff`, meaning "the range's own size"; the script requires
  exactly that value.

Verified:
- IVFC: levels 1, 1, 1, 2, 686 and 350,852 data blocks, all pass. One flipped byte from the update data fails level
  4 block 3, one flipped byte from the base fails level 5 block 13 (negative controls).
- Compression: 98,996 ranges (96,813 LZ4, 2,183 stored, 0 zeros); every LZ4 block decompresses to exactly its range
  size. The RomFS header and file table then parse: **10,965 files, 8,036,390,636 bytes**, SHA-256 `6854222f…`.
  5.60 GB of stored bytes come from the base, 0.15 GB from the update. The decompressed bytes themselves have no hash
  of their own; LZ4 and size errors stop the script.
- Content: 1,655 `.bfres`, 953 `.bntx`, 96 `.bfsha` (precompiled shader archives), 180 `.bfstm` + 1 `.bfsar`
  (NintendoWare audio), 19 `.webm`, 93 `.arc`; folders `region_common` and `regionCN/EU/JP/KR/TW/US`.

**Rewrite.** `patch_nso.py` with the build's variables (`DM_RULESET=native DM_PRIVATE_GATES=0`) into
`rewritten/v110`. The bytes match the stage-0 rewrite exactly (deterministic). No new site kind was needed.

| Module | Sites | Notes |
|---|---|---|
| main | 6,121 | 5,543 x18, 462 `tpidr_el0` reads, no svc |
| sdk | 15,793 | 58 svc, 43 cntpct, 101 host gates |
| subsdk0 (GLSLC) | 4,855 | x18 only |
| subsdk1 (movie) | 9,784 | x18 only |

- `structck.py`: OK for all four modules, 0 violations (`LLVM_BIN=/opt/homebrew/opt/llvm/bin`; Homebrew now ships
  llvm 23 as `llvm`, not `llvm@23`).
- `equiv.py`: 36,452 sites, 0 divergent, every site with at least 2 effective trials; 1,200 of 1,200 mutation
  controls caught (300 per module). The 101 sdk gates are checked by structck only, as designed.

`assets/expected-v1.1.0.json` pins the ExeFS, rewrite and RomFS hashes.

Pitfalls:
- `equiv.py` needs `unicorn`, which `requirements.txt` does not list. The work folder's venv (Python 3.14) has it.
- Do not read `romfs.bin` for the hash tree: IVFC covers the stored (compressed) image only.

### 2026-10-24: stage 2, the game reaches `nnMain`

`games/eow` is a copy of `games/by3` (its own runtime, rewriter, NVN-on-Metal layer and prebuilt translator).
Changes:

- `game_version.h`: title `01008CF01BAAC000`, version 1.1.0, folder `v110`; still no game address defined.
- Four modules instead of three: `scripts/prepare_runtime.py` builds tables for main, subsdk0, subsdk1 and sdk (the
  loader iterates `modules[]` and needed no change). DT_INIT has TotK's shape in all four (x19, +0x10 / +0x14).
- Names: `BY3_*` variables became `EOW_*`; `runtime_by3_override` became `runtime_game_override` (the by3
  network/system answers are kept, since EoW links the same libraries); window and menu titles.
- Removed: by3's launcher app (`app/`, the `app` / `install` targets) and `make_res_patch.py` (Bayonetta addresses).
  Kept but unused: `prepare_patches.py` (no EoW patches yet).
- Kept from by3's runtime defaults: `DM_POOL_BUFFER_MAX_MIB=2048`, `DM_PRESENT_HZ=60` with `TOTK_UNCAPPED=0`,
  `DM_PRESERVE_INVARIANCE=1`, `DM_NVN_GPU_CODE_MAX_MINOR=31`. None is measured on EoW yet.
- System fonts: the TotK build's extraction from the same console, copied to `extracted/firmware/shared-fonts`.

Verified:
- `eow-native rewritten/v110 probe`: pass. Four modules mapped, 577,419 relocations, 4,000 calls main PLT → sdk →
  host timer on 4 threads.
- `TOTK_HEADLESS=1 TOTK_VERIFY_STARTUP=1 … boot`: the original `nn::init::Start` ran, the RomFS (8.0 GB) verified,
  every constructor finished with state 2 (main 1,486, subsdk0 8, subsdk1 45, sdk 83 init_array entries), and the
  original `nnMain` ran. The startup checkpoint's "focus setup" stop is TotK / Bayonetta-shaped and is not reached.

### 2026-10-24: stage 3, to the first `nvnQueuePresentTexture`

Method as in by3: boot (headless until VI needs a window), read where it stops, fix, repeat. New tools:
- `scripts/disasm.py main+0x… [n]`: original image, PLT calls and exported functions named.
- `scripts/xref.py` (code forming an address), `scripts/callers.py` (direct calls).
- `scripts/lldb/`: lldb Python callbacks (module bases, guest frame-pointer walk, register logging at chosen main
  offsets, written to `logs/lldbcb.out`). In `lldb --batch`, plain `breakpoint command add` and watchpoint command
  output was lost; Python callbacks writing to a file work.
- `TOTK_FS_LOG=2`: also every RomFS open result, read, size, close and entry-type query (`fs_host.c`).
- `<work>/run.sh [seconds]`: a boot with a time limit (`HEADLESS=1` for headless).

Answered, in order of appearance:

1. **`nn::oe::GetNotificationMessageEvent`** (`applet_host.c`). EoW waits on it with a MultiWait; the host answers
   `oe::Initialize` itself, so the SDK never made the event and `InitializeMultiWaitHolder` aborted. The event is now
   made on the first request (the SDK's own `CreateSystemEvent`, via `hle_bind_system_event`). It is signalled while
   messages are queued; the pop that empties the queue clears it, under the queue lock so a later push cannot be lost.
2. **`nn::oe::Enter/LeaveExitRequestHandlingSection`** (no-ops: the host has no system exit request) and
   `IsUserInactivityDetectionTimeExtended` / `GetUserInactivityDetectionTimeExtensionMode`. Without them the SDK
   called through a null `am` self controller.
3. **`nn::hid::BindNpadStyleSetUpdateEvent`** (bound and signalled once: the controller is there from the start),
   `GetNpadJoyHoldType`, `SetNpadJoyAssignmentModeDual`, `SwapNpadAssignment` (`input.c`). The first reached `hid`
   over IPC, which the host does not serve.
4. VI needed a window from here on. **`nn::vi::SetLayerCrop`** (recorded and logged, not applied: the game sets the
   full 1920x1080), `DestroyLayer`, `CloseDisplay`, `Finalize` (`frontend.m`).
5. **C stdio on RomFS** (`runtime/stdio_host.c`, new). This one took the longest. The game crashed in its vibration
   manager (`main+0x16c46d4`) on an empty resource for `vibration/vibration.vibs`. Followed with lldb:
   - the resource's job counter was 0 and its value never set;
   - the loader job (`main+0x2737f68` → `…2743390`) opened the file through `nn::fs`, read its size, closed it, and
     never read it;
   - it then asks a stream provider (`main+0x2743410`) for a `std::shared_ptr<std::istream>`, which got null back.
     The provider builds a libc++ `std::ifstream`, whose `basic_filebuf::open` calls **`fopen`**.
   - The SDK's own `fopen` (musl-style: `open` + `fdopen`) cannot open `rom:/…`, because the host answers the RomFS
     mount itself. TotK and Bayonetta 3 only used `nn::fs`.

   Fix: `fopen`, `fread`, `fwrite`, `fseeko`, `ftello`, `fflush` and `fclose` are replaced **for main's imports
   only**, through a new per-module hook (`runtime_module_override`, called in `loader.c` before the global overrides).
   A `rom:` path opens a read-only host stream; other paths, and every `FILE*` the host did not make (stdout, stderr),
   go to the SDK's functions unchanged. The SDK, GLSLC and movie modules keep their own stdio. After this, textures
   (`.bntx`) and shader archives (`.bfsha`) load, and NVN work starts.
6. **Runtime shader translation**: `make shader-tools` (as by3: the repo's prebuilt `totk_xlate`, SHA-256 checked,
   and SPIRV-Cross at the pinned commit, built in the work folder).

**Result.** Booted with a window and GPU submission off (`TOTK_GPU_ALLOW_SUBMIT` unset: work is encoded, never
submitted, so the GPU driver is not exercised):
- frame 1: `nvnQueuePresentTexture` (35 NVN calls);
- frame 2: `QueueSubmitCommands`, `QueuePresentTexture`, the first program (one vertex and one pixel shader,
  translated by the runtime service from the game's precompiled NVN binaries).

**GLSLC.** No runtime GLSL compile has happened so far. The shaders come from the 96 precompiled `.bfsha` archives
and go through the TotK translator like TotK's. `subsdk0` (GLSLC) is mapped, relocated and constructed, so a later
`glslcCompile` call would at least run; whether its output (NVN GPU code) fits the translator is untested.

Seen, not handled yet:
- `贴图→贴图拷贝未实现的形态` (texture → texture copy, 1920x1080, format 92 → 71): a copy shape the Metal layer does
  not implement.
- `nn::fs::CleanDirectoryRecursively`, `nn::nfp` after mount, `nn::settings` debug pad map: imported, not answered;
  not reached.

**Next blocker** (where it stops now, deterministic): right after frame 2, `nn::mem::NumberLineAllocator::Allocate`
crashes in the SDK (`sdk+0x13b750`, reads address -1). Caller `main+0x27a57ac`, in a GPU-memory sub-allocator set up
by `main+0x27a5400` (from `main+0x278c304`). It asks for **0 bytes**: the size is the game's own field
`[obj+0x18]` (alignment 0x200 from `main+0x325d94`, which is fine). 473 earlier allocations from the same allocators
succeeded. Next step: find what fills `[obj+0x18]` (the object comes from the virtual call at `main+0x278c2f8`). The
suspect is a host answer that is 0 where the Switch gives a size, most likely an NVN query (`nvnDeviceGetInteger`, a
builder `GetStorageSize` / memory-requirement getter) or a value read from a RomFS file.

After that, still ahead: GPU submission (`TOTK_GPU_ALLOW_SUBMIT=1`; macOS GPU panics were seen in the Odyssey port, so
watch the first submitted runs), audio (`audren:u` is answered; EoW also opens `AudioOut`, which by3's
`audio_out.c` covers), movies (`.webm` through `subsdk1`), saves, input mapping, the full-frame VI crop.

### 2026-10-24 (afternoon): from the 0-byte allocation to 10,000 frames with sound (GPU submission still off)

Each item: what stopped the game, how it was found, the fix.

1. **0-byte GPU allocation** (`NumberLineAllocator::Allocate`, the old "next blocker"). Logging every
   `nvnDeviceGetInteger` showed only `SUPPORTS_DEBUG_LAYER` = 0 (correct). The size came from a function pointer the
   game had taken with `GetProcAddress` (`main+0x3487e00`): a host thunk, index 353 = **`nvnTextureGetZCullStorageSize`**,
   which the graphics layer answers with 0 (Metal has no ZCull). EoW allocates each depth texture's ZCull save area from
   its own allocator. `DM_NVN_ZCULL_STORAGE=<bytes>` (default 0, TOTK); the EoW runtime sets 256, the allocator's unit.
   Save / restore stay no-ops.
2. **Text: no glyph for U+25A1 in any font.** A font-set lookup (`main+0x8e260`) asked
   `nn::fontll::ScalableFontEngine::CheckGlyphExist` and all six system fonts said no. The host handed the game the BFTTF
   files as stored: past the 8-byte header every big-endian word is still XORed with the key (word 2 decodes to
   `0x00010000`, the TrueType magic; word 1 to the font size). In the Switch's pl shared memory only the header is
   obfuscated. `shared_font.c` now decodes the payload after the hash check and checks the TrueType magic. With fonts,
   the game opens `audren:u` (Core Audio, 48 kHz stereo), initializes vibration, mounts its save and finishes its
   startup logo.
3. **Compute shader 6f999f70…: a texture binding without a slot** (`cp_t_tcb_48`), fatal in the reflection loader.
   The shader only calls `get_width` / `get_height` on handle 0x48, which is its own image `cp_i_cb2_48_` (driver bank
   2, +0x48). `nvn_refl.m` binds such a `cp_t_tcb_<h>` (no slot, and an image `cp_i_cb2_<h>_` exists in the same
   shader) as that image, and its sampler as sampler 0. It is the only such shader in the cache so far.
4. **`nvnCommandBufferSetCopyImageStride`** (unimplemented, fatal): recorded with the row-stride op (`p_copy_stride`
   gained an `image` flag); buffer <-> texture copies use it as the layer / slice stride (0 = tightly packed, as before).
5. **Cube-face views asking for more mip levels than exist** (7 levels from base 1..6 of a 7-level cubemap, refused
   99,000 times in 2,400 frames): the level count is clamped at the texture's last level, as the hardware descriptor
   does. Any other invalid view is still refused.

Result (window, `TOTK_GPU_ALLOW_SUBMIT` unset): the game ran the full 3-minute limit, about 10,000 frames, frame time p50
16.66 / p95 16.80 ms, no crash; audio output level rms 0.05, which matches the user hearing the game. No picture yet,
because nothing is submitted to the GPU.

Then, for the first picture:
6. **2D-engine copies a blit cannot do**: a format conversion RG11B10Float -> RGBA8 sRGB at 1920x1080 (the HDR scene
   into the presented image) and a scaled copy 1408x792 -> 1920x1080 (dynamic resolution). `copy_t2t_convert`
   (`nvn_mtl.m`): one triangle per slice over the destination rectangle, sampling the source in texel coordinates
   (nearest, or linear with the copy's linear-filter flag); only float / unorm colour formats and a render-target
   destination. Not yet verified on the GPU.

Also: `EOW_QUIET_CRASH=1` (set by `run.sh`) ends an expected abort with `_exit`, so test runs do not open macOS crash
report dialogs (the user saw one).

**Open, found on the way:**
- **Movies**: EoW creates a VP9 decoder in **mode 2** (output to a native texture, format 3). The host decoder
  (`movie_host.c`, from TotK) covers only mode 1 (buffer), so mode 2 goes to the library's own decoder, which needs
  Tegra NVDEC: an SDK Break in `NvRmMemHandleAlloc` from `android::SfNvnUtil::AllocateGrallocBuf`. One run of five
  reached it (a movie starts later in the others' 3 minutes or not at all; not established which movie).
- `nvnSyncGetNvRmSyncNVX`, `nvnTextureGetNvRmSurfaceNVX`: looked up, unknown to the layer; reported if ever called.

### 2026-10-24 (late afternoon): GPU submission on, the title screen

With the user's go-ahead (Odyssey's port had seen macOS GPU panics): `TOTK_GPU_ALLOW_SUBMIT=1` plus by3's launcher set
(`TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BUDGET_MB=5000 DM_PRESENT_LINK_FALLBACK=1`), short
runs with `TOTK_INPUT_FILE` shots. No GPU fault in any run.

- **Movies, mode 2**: `movie_host.c` now takes VP9 decoders in mode 2 and output format 3 as well (the game, at
  `main+0x1897040`, reads `width`, `height`, `nv12-y-stride`, `nv12-y-offset`, `nv12-uv-offset`, `nv12-colorspace`
  from each frame's MediaData, all of which the host sets). The library's own NVDEC path, and its Break, are no longer
  reached. **Not finished**: in mode 2 the game hands `GetOutputBuffer` an **8-byte** buffer (4,890 "output buffer of
  8 bytes for a 3110400-byte frame" in 16 s), so it wants a handle (most likely a texture), not pixels. The title's
  moving background is that movie; it stays black until this is answered.
- **The conversion copy at first gave black.** `texdump` of the frame showed the HDR target (ser 3, RG11B10Float) with
  the full title screen; a magenta-only variant (`DM_EXP_CONVERT_MAGENTA=1`, kept as a measurement knob) proved the
  draw reached the presented image; `read()` instead of `sample()` showed the picture. The sampler bound through the
  MTL4 argument table read back zeros (not investigated further). The copy now reads texels only: nearest, or bilinear
  from four reads. Also: one argument table per copy from a ring of 64 (the GPU reads a table when the draw runs; the
  frame's scale copy and convert copy shared one).
- **Result**: the title screen, "塞尔达传说 智慧的再现 / THE LEGEND OF ZELDA" and "Press Any Button", presented in the
  window (`debug/title.png`), with sound.

Seen on the title, open:
- "Press Any Button" is **upside down** (rows flipped) with a faint green fringe: the text texture the game renders on
  the CPU with fontll (1564x127 / 1500x63 RGBA8 textures) is uploaded or sampled with its rows in the wrong order.
- The movie background (mode 2 output handle, above).
- `nvnSyncGetNvRmSyncNVX` / `nvnTextureGetNvRmSurfaceNVX`: looked up by the game, unknown to the layer (likely the
  movie path's way to share textures with the decoder).
- Input past the title has not been tried.

### 2026-10-24 (evening): the title movie

- **What mode 2 hands the decoder.** A diagnosis in `GetOutputBuffer` read the 8 bytes the game's Buffer points at: the
  low word was `0x00584554`, the graphics layer's `'TEX\0'` magic. The Buffer's data pointer **is the game's
  NVNtexture** (1920x1080, NVN format 0x38 RGBA8 sRGB, flags video-decode | image, block-linear); on the Switch the
  video engine writes the frame into it as RGBA. `movie_host.c` now converts each decoded frame (BT.709 limited range,
  8-bit fixed point) and writes it with the layer's WriteTexels path (`dm_nvn_write_texels`, a small export in
  `hle_nvn.c`). Other textures (format or size) are reported, not written.
- **Leak**: that upload makes autoreleased Metal objects on the game's thread, which has no autorelease pool; each frame's
  ~16 MB stayed allocated and after 300 frames the GPU budget guard (5,000 MB) stopped the game. The upload now runs
  inside `objc_autoreleasePoolPush` / `Pop`. After the fix: 40 s, 2,100 movie frames, GPU memory steady at 1,049 MB.
- **Result**: the complete title screen, the Hyrule panorama movie behind the logo, colours right
  (`debug/title_movie.png`). VP9 decode 0.55-0.75 ms per frame; the RGBA conversion is scalar C (not measured; NEON if
  it shows up).

Still open on the title: "Press Any Button" upside down (CPU-drawn text texture, rows reversed). Input past the title
not tried.

### 2026-10-24 (evening): the upside-down text was the window origin

- **Cause.** "Press Any Button" is drawn in three render-to-texture passes (pass dump: glyphs instanced into a 1500x63
  texture, a texture copy, a pass into a 1564x127 R8 mask, a pass composing it into RGBA), then sampled onto the
  screen. EoW never calls `nvnDeviceSetWindowOriginMode`, so it keeps NVN's default **lower-left** origin (the OpenGL
  convention: the top of a rendered picture is its last row in memory); TotK sets UPPER_LEFT (its log says so), and
  the graphics layer ignored the mode. On Metal every render-then-sample step flipped the picture once; three steps
  left the text upside down, while the logo (an ordinary texture) and the movie were right.
- **Fix** (`nvn_mtl.m`, `hle_nvn.c`): with the lower-left origin (`g_nvn_origin_upper` = 0, NVN's default), game
  draws use a vertically flipped viewport (negative height), so render targets get the Switch's memory layout; rows,
  so copies, clears, scissors and fragment coordinates, keep the game's numbers. Front-face winding is inverted (the
  flip reverses it). The picture is flipped back when shown (both present paths) and in `shot` captures.
  `DM_NVN_ORIGIN=upper` turns it off for comparison. TOTK, which sets UPPER_LEFT, is unaffected.
- **Verified**: `shot` (the game's presented texture) and `wshot` (the window as shown) of the title: logo, movie and
  "Press Any Button" all upright; the text's green fringe is gone too (`debug/flip_shot.png`, `debug/flip_wshot.png`).
  No Metal error with negative viewport heights.
- Not checked yet: `texdump` render-target dumps now show the game's memory layout (upside down for viewing); MetalFX
  output paths (`TOTK_MFX_*`, off here) and the FPS overlay were not adjusted for the flip.

Next: input past the title (keyboard or the connected Xbox controller), into the game.

### 2026-10-24 (evening): into the game; the dark picture; stutter

- **Past the title.** Scripted presses (A = key J, the stick = WASD) go from the title into the playable prologue:
  Link in the dungeon, hearts and the Y / X / A item HUD, Chinese prompts ("Y 挥剑", "B 放下 / A 投掷" when lifting
  a pot), torches and plants; walking works. 100 s without a crash (`debug/play2`).
- **Two more 2D-engine copies**: `MIRROR_Y` (flag 0x8; 92 -> 70 at 1920x1080, probably a capture) through the
  conversion copy with a negative step; and a **depth texture at half size** (Depth32Float_Stencil8 1408x792 ->
  704x396, no filter) through a depth-only draw (`copy_t2t_depth`: depth always passes and is written; stencil not
  copied).
- **The picture was too dark** (the user: darker than the console). The game's presented texture (`shot`) had the right
  brightness, the window (`wshot`) did not: EoW's window texture is RGBA8 **sRGB** (TotK's is not), sampling it gives
  linear values, and the present pass stored them into the plain BGRA8 drawable. The present shader now encodes linear
  -> sRGB when the source is sRGB and the drawable is not (`g_blit_pso_enc`, both present paths). After the fix the
  window and the texture match (`debug/bright`).
- **Stutter**: frame intervals are a steady 16.7 ms except first-time shader / pipeline builds (one 100 s run: 57 long
  frames, 31 of them building pipelines, 24 over 100 ms). The layer's automatic collection already records them
  (`pso_user.tsv` / `pso_collected.tsv`, 380 rows after a few runs) and prewarms them in the background at the next
  start (174 pipelines), so a second pass through the same places is smoother; new places still hitch once. Not done:
  a shipped seed of the whole game's pipelines (TotK has one).
- **Controller**: the user played with their Xbox Wireless Controller (GameController, the TotK host's support) and it
  was recognised and worked in game (2026-10-24).

### 2026-10-24 (evening): stutter causes; audio clicks (volume ramps, depop)

User report after playing: frequent stutter, also in places played before; long room transitions; the music crackles
("哒哒哒").

- **Stutter in places already played**: the shaders are cached as MSL, and macOS caches their compiled code, but that
  cache is per executable: every rebuild of `eow-native` started it empty. Controlled pair (same binary, same scripted
  route twice): shader library compile 56,118 ms in the first run, 1,400 ms in the second. A shader never seen before
  still costs 300-600 ms once (the large ones, 40-115 KB of MSL). Room transitions run at ~30 fps for a few frames
  (loading); not investigated further. New: `DM_LOG_SHADER_LOAD=1` logs each MSL compile with its time.
- **Clicks**: a 30 s capture of the game's mix (`TOTK_AUDIO_CAPTURE`) showed abrupt steps on the music's 0.3 s grid:
  a voice at 0.12 stopping dead (0.117 -> -0.001) and later notes starting mid-waveform. The renderer mixed every frame
  (240 samples) at a constant volume and cut stopped voices to zero; the Switch's renderer ramps volume and mix volumes
  linearly across each frame from the previous frame's values, starts a new voice's volume from 0, and decays a stopped
  voice's last sample into the mix (depop; per sample x0.962189 at 48 kHz, x0.943695 at 32 kHz). `audio_mixer.c` now
  does the same for the voice -> channel -> mix path (EoW uses no splitters); a re-acquired voice (`is_new`) drops its
  old tail. After the change the stop-to-zero steps are gone from the capture; sharp onsets remain on the beat grid,
  which may be the music's own percussion (to be judged by ear). `probe-audio` passes; it runs flat (no ramps / depop:
  its fixtures predate them, `g_audio_probe_flat`).
- `<work>/play.command`: double-click launcher with the GPU settings, a log per session (`logs/play-<time>.err`).

### 2026-10-24 (evening): the music crackle was end-of-stream handling

The user: crackle ("哒哒哒") whenever music plays, none with sound effects only.
- EoW's music is streamed DSP-ADPCM (`.bfstm`: codec 2, 48 kHz, 2-6 channels), queued as 14,336-sample blocks
  (0.2987 s), and the game marks **every block end-of-stream**. The renderer (`advance_voice`, `skip_empty`) stopped
  the frame at an end-of-stream buffer and the mixer's walk ended there, so up to one frame (240 samples) of each
  block boundary was silence: a click every ~0.3 s in all music. `TOTK_AUDIO_STEP_TRACE=1` (new: a voice whose first
  sample of a frame jumps far beyond its in-frame changes) showed it directly: the frame before each block ended in
  zeros. As on the DSP (and Ryujinx / yuzu), end of stream now only resets the played-sample count and playback goes
  on into the next queued buffer; with none queued the voice ends as before.
- Ruled out on the way: ADPCM history across blocks (the blocks carry no context; the carried history continues, no
  rewinds, `TOTK_AUDIO_ADPCM_TRACE=1`); the bfstm SEEK history differs by a few LSB from the decoded one in every
  block, which looks like the encoder's source PCM, not a decode error (`debug/bfstm_check.py`).
- Verified: 30 s capture in the dungeon, 0 step events and 0 detected clicks (before: 129 / 12); `probe-audio` passes;
  the user hears no crackle (2026-10-24).

### 2026-10-24 (night): shaders compiled ahead of time

- **Why**: macOS caches runtime shader compiles per executable, so every rebuild of `eow-native` brought the whole
  warm-up back (56 s of compiling in the first run after a rebuild, see above).
- **Runtime** (`nvn_mtl.m totk_msl_library`): every library compiled from MSL, translated shaders and patched
  variants alike, is first looked up in `<work>/build/shader-libs/<SHA-256 of the MSL>.metallib` (TotK's prebuilt
  lookup, which only base shaders used, now covers all of them); on a miss the MSL is kept in `shader-libs/src` for the
  next offline build (`DM_SHADER_LIBS_RECORD=0` turns that off). `shader-libs/flags.txt` names the offline options; if
  they differ from the run's (safe math, `DM_PRESERVE_INVARIANCE`) the folder is ignored and reported.
- **Offline** (`scripts/build_shader_libs.py`): compiles the translation cache's MSL plus the recorded sources with
  `-fmetal-math-mode=safe -fpreserve-invariance` (the runtime's options), in parallel, skipping what is built.
  First build: 1,392 libraries in 22 s, 0 failures. `play.command` runs it before each start (1.7 s when little is new),
  so shaders first met in one session are precompiled for the next.
- **Verified** right after a rebuild (macOS cache empty): the warm-up loaded 801+ libraries precompiled and compiled 0
  ("着色器库：预编译加载 801 个，现编 0 个"); 871 libraries in 0.75 s in total, against 56 s before.

### 2026-10-27: black window at start after long play

The user saw a black window at start and closed it. The log: the pipeline warm-up holds the first frame until its first
list is built, which in TotK is the title's own list; EoW has none, so its first list (`pso_user.tsv`) holds every
pipeline collected while playing (3,137 rows, 1,664 pipelines after a few hours): 5,055 ms of black window, then the
game ran normally. `DM_WARM_TITLE_WAIT_MS` (new; default: wait until done, TOTK unchanged) caps that wait; the EoW
runtime sets 1500. The rest is claimed at later submits as it finishes (`warm_claim` is incremental), and save-load
still waits for the warm-up (`mtl_warm_wait`). Measured after the change: 588 ms (Metal's pipeline cache warm), frame
121 three seconds after the window opened.

### 2026-10-27: a crash in early sessions was `nn::fs::CleanDirectoryRecursively`

Two of the play logs from 2026-10-24 (17:26 and 17:28) end in `sm: service "fsp-srv" not implemented`. Callers
(`scripts/symbolize.py`): main+0x2384888 calls `nn::fs::CleanDirectoryRecursively`, which the host did not answer, so
the SDK went to the real file system service. The game accepts success or path-not-found (it checks for 0x202).
`fs_host.c` now answers it like `DeleteDirectoryRecursively` but keeps the directory: every entry under it is removed
(through the same path mapping, so it can only touch the save / cache folders in the work folder). `probe` and
`probe-audio` pass after the change; the later sessions in the logs (up to 142,500 frames, about 40 minutes) never
reached that call, so it has not been seen answered in play yet.

### 2026-10-27 (evening): crash entering the first cave

The user's game closed on entering a cave in the first dungeon (log `play-20261027-193240.err`, frame ~8,100). SIGBUS,
a write permission fault at 0x7958bcf5c8, pc in the host's `nvnBufferFinalize` (`memset` of the game's NVNbuffer),
called from a destructor at main+0x325fec during the area change. (The host's load slide was found by matching the
thread-entry return address in the backtrace against `otool` output, then `atos`.) The fault handler did not know the
page, so it was not one of the dirty tracker's live pages.

Likely cause, in the dirty-page tracker (`nvn_dirty.c`), which write-protects NVN pool memory: (1) a protect that races a
pool's destruction can leave pages read-only after the destruction unprotected them, with no live range; the memory goes
back to the game's heap and the next write there faults; (2) a pool created inside another pool's range is tracked by
that range (the log shows one at start), and destroying the inner pool removed the outer pool's range. Changes:
destruction only removes the pool's own range, unprotects the whole range again after marking it dead, and records it
in a ring of 1,024 destroyed ranges; a protect re-checks that the range is still live after its `mprotect`; a write fault
in a destroyed range (or on a never-written page of a new range over one) makes the page writable and continues. The
periodic texture line counts these repairs. Not confirmed: when the crash report was written the page was already
writable again (region protection rw), so the exact interleaving is not proven; the cave is the test.

Result (`play-20261027-195130.err`): the user went into the cave again, no crash, 12,300 frames. The repair counter
stayed at 0, so the fallback was never needed in that session: either the two structural fixes (destruction removing
only its own range; the protect re-check) removed the cause, or the race did not occur this time. Kept all three.
