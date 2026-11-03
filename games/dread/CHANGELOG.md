# Metroid Dread port: work log

A record of how this port is being made, for anyone who continues it or tries the same with another game. Each entry
says what was found, what was done and why, what went wrong on the way, and how it was checked. Newest entries go at
the bottom of [Log](#log); [Status](#status) is kept current. The Bayonetta 3 log (`games/by3/CHANGELOG.md`) is the
method this port follows.

Conventions: dates are absolute; "verified" means a check with a recorded result, never "it looked right"; commits are
named by hash and subject. Game data, keys and saves are never committed (see the Metroid Dread block in `.gitignore`).

## Status

| Stage | Goal | State |
|---|---|---|
| 0 | Feasibility: does the game fit the TotK architecture? | done 2026-10-25 |
| 1 | Code and data out of the dump; code rewritten for Apple silicon | done 2026-10-25 |
| 2 | A runtime target with the other games' parts compiled out, booting to `nnMain` | done 2026-10-25 |
| 3 | Follow every unknown service / command / NVN call until the first `nvnQueuePresentTexture` | done 2026-10-25 (first presented frame at 1.2 s) |
| 4 | A picture, audio, input, saves | in progress: a new game reaches the first area (Artaria) with HUD and minimap |
| 5 | Performance, shaders, packaging as an app | in progress: Mac app; iPhone / iPad app (2026-11-01), its shader seed (2026-11-02) |

## Log

### 2026-10-25: stage 0, feasibility

Input: the base game card image `010093801237C000` v0 (1.0.0, 4.4 GB XCI), `prod.keys` from the same Switch. The card
holds four NCAs (Program, Control, Legal, Meta), all of the base title: no update, no sparse sections.

Findings, each read from the dump:

- **Same module layout as TotK and Bayonetta 3**: `main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`. SDK 11.4.3 (nnSdk, NVN,
  movie, libz, NintendoWare Atk). main build ID `49161D9CCBC15DF944D0B6278A3C446C006B0BE8`.
- **Graphics are NVN.** main loads 524 names; 523 are in the graphics layer's table (the other is
  `nvnBootstrapLoader` itself), 425 have an implementation in one of the three forks. 46 of the missing ones are not
  getters (transform feedback, downsample, `ReportValue`, integer clears, tessellation levels, ...): a call aborts
  with its name, so stage 3 lists the ones the game uses.
- **Audio** is NintendoWare Atk through `nn::audio` (149 imports), not Wwise. **Movies** use Nintendo's movie library
  (subsdk0 carries the WebM parser); the title screen, the prologue and the loading trains are `.webm`.
- **Data**: Lua 5.1 scripts and `packs/*.pkg` archives.
- **The npdm allows 47 services**, a set close to Bayonetta 3's (`nifm`, `bsd`, `ssl`, `friend`, `bcat`, `prepo`, ...).
- **The rewriter fails on main.** sdk and subsdk0 rewrite with the generic scan; main stops with an out-of-range branch
  (`AssertionError` in `B()`). main's `.bss` is `0x70ccf788` bytes (1.8 GiB). The patch area goes after the image
  (`PATCH_OFF = page_up(text + rodata + data + bss + key page)`), about 1.93 GB from the text, and a site's `B` reaches
  ±128 MiB.

### 2026-10-25: the fork, stage 1

**Layout.** `games/dread` is a copy of `games/by3` (commit `6142be3` is the unchanged copy, so the following commits'
diff is the whole fork). Bayonetta 3 is the newest fork (`site_spset`, CMIF domains, `audout:u`, `set`, offline
network services, RomFS listing). The work folder is `games/Dread_build`, next to the card image.

**Code and data.** The fork's `extract_exefs.py` / `extract_romfs.py` (TotK's, as Bayonetta's copies) select the base
Program NCA of `010093801237C000` without BKTR and refuse anything else. Bayonetta's sparse-RomFS merge, update-code
extractor and 1.2.0 patch scripts are removed.

Verified: ExeFS FS header, hash table and every block; RomFS all six IVFC levels (268,575 data blocks; 9,654 files,
4,400,319,820 bytes, SHA-256 `d3c6c310…`, matching an independent `shasum`); one byte flipped in a level-5 read fails
block 17,408. `assets/expected-v1.0.0.json` pins all of it.

Commit: `5079033` feat(dread): extract and verify the code and RomFS of Metroid Dread 1.0.0.

### 2026-10-25: the leading patch area

**Rewriter.** When the patch area after the image would be out of branch reach (`PATCH_OFF >= 2^27 - 1 MiB`),
`patch_nso.py` places it `PATCH_LEAD` (4 MiB) before the module, growing towards offset 0. The key page stays after
`.bss`: ADRP reaches ±4 GiB. The file holds the patch area, padding to `PATCH_LEAD`, and the image up to the end of
data; `.bss` and the key page are zero-filled by the loader (otherwise the file would be 1.9 GB and wholly read on
every start). `fixups.tsv` gains `PATCH_LEAD`, `PATCH_SIZE` and `IMAGE_SIZE`. The encoders already did signed
arithmetic. A first lead of 32 MiB was cut to 4 MiB once main's patch area turned out to be 43 KB (1,085 sites: main's
code rarely uses x18); an assert stops a module whose patch area does not fit.

**Checkers.** `nsoimg.Patched` reads a rewrite by module offset in either layout (zeros past the stored bytes).
`structck.py` and `equiv.py` use it. equiv maps the same range in both emulators and keeps `.bss` out of Python memory.
capstone reports a negative address as unsigned; equiv's `plain_block` and structck's `keyseq` / `paramseq` convert it
back (main has no gates or parameter windows, so structck passed before that fix too).

Verified:

- sdk and subsdk0 rewritten by this fork are byte-identical to the `games/by3` rewriter's output (patched image, gates,
  sites, cuts); `fixups.tsv` differs only by the three new header lines. Two rewrites give identical bytes.
- `structck.py`: OK on all three (main 1,085 sites, sdk 12,085, subsdk0 10,486), the same counts as the unmodified
  checker on sdk / subsdk0. A main copy with one flipped patch word fails (key-slot sequence and slot count), one with a
  non-zero pad byte fails (pad check).
- `equiv.py`: 0 divergent sites on all three; sdk and subsdk0 results identical to `games/by3`'s equiv. Mutation
  control: sdk and subsdk0 300/300. main with seed 1 caught 299/300: the miss is a nop in place of
  `csel x18, x12, x18, eq` at `0x17b008`, which only shows when `eq` holds, and its 3 trials had Z clear. Seeds 2, 3
  and 4 catch 300/300. Recorded as a limit of the mutation control, not changed.

Commit: `8873af7` feat(dread): the rewriter places the patch area before a module whose image is out of branch reach.

### 2026-10-25: the loader, and `nnMain`

**Loader.** `Module` gains `patch_lead`, `patch_size`, `image_size`; `patch_off` is signed. The Mac `map_module`
reserves `patch_lead + image`, reads the file from the reservation's start (the module base is `patch_lead` in) and
checks the file's SHA-256 as before. `seal` makes the lead range RX; `check_permissions` and the page classifier know
it. Crash lines, caller traces and `runtime_module_offset` name a lead address `<module>.patch+offset`. The iOS loader
refuses the layout.

**Fork identity.** Title `010093801237C000`, version 1.0.0 / `v100`, `dread-native`, `org.macnativegames.dread`,
`DREAD_*` environment names, window and menu titles. BY3's 1.2.0 patches, `build.py` and `game.json` are removed
(`build_app.py` reads every `games/*/game.json`; the copied one named Bayonetta 3 again). The defaults
`system_services.c` sets for the graphics layer (pool size, invariance, GPU code versions, 60 Hz pacing) are Bayonetta
3's and are kept until Dread's own are measured. Comments that say which game needed a graphics-layer feature keep
naming Bayonetta 3.

Verified:

- `make prepare`: 198,378 relocations; constructor-state word found the same way in all three modules (adrp x20 at
  +0x10, ldr at +0x18); main's is in `.bss` (`0x1cee0d0`). `make runtime` builds with `-Werror`.
- `make probe`: three modules mapped (main 1,922,859,008 bytes with `.bss`), sealed and permission-checked;
  4,000 main PLT → sdk → host-timer calls on 4 threads. A main image with one flipped patch byte is refused
  ("image SHA256 differs from verified rewrite").
- Headless boot (`TOTK_HEADLESS=1 TOTK_VERIFY_STARTUP=1`): `nn::init::Start` ran the SDK startup on the verified RomFS;
  every constructor finished with state 2 (main 2,751, subsdk0 44, sdk 115 `init_array` entries); the game's `nnMain`
  ran and brought up the Metal device, then stopped at `sm: service "fsp-srv" not implemented` (caller
  `main+0x1910` → `main+0x132b28` → `main+0x13241c` → sdk). Whether one of main's rewrite sites ran on the way was not
  measured.

Not built or run: iOS, the launcher and `make app`, any picture.

Open questions carried forward: why this SDK opens `fsp-srv` over IPC where TotK's and Bayonetta's are answered by
function name; which of Bayonetta's offline services and graphics defaults Dread needs; whether the Atk audio path
matches TotK's audio renderer; the system fonts (the card carries none).

### 2026-10-25: stage 3 (in progress), from `nnMain` to the display

Method as for Bayonetta 3: boot headless, read where the host stops, name the SDK frames (`scripts/symbolize.py`),
fix the platform layer, repeat.

- **`fsp-srv`** was not a missing service: main's first call is `nn::fs::SetAllocator`, which no other port's main
  makes. The SDK's own fs library then checks its access log and opens `fsp-srv` over IPC. The host answers fs by
  name and never allocates guest memory for it, so `SetAllocator` keeps nothing. `DeleteDirectory`, also imported, was
  added (an empty save folder).
- **`nn::oe::EnterExitRequestHandlingSection`** (a null applet object in the SDK's own version): the host never sends
  an exit request, so entering or leaving the section is a no-op.
- **An audit instead of one crash per boot**: main imports 261 SDK functions that no host override answers. Almost
  all are library code on the SDK's own system calls (`nn::os` 53, `nn::util`, `nn::mem`, the `nn::audio` client
  whose service `audren:u` the host answers over IPC, vibration players, play-report builders). Those that would reach
  an unanswered service: the two `oe` sections above and 12 `nfp` calls, now "no device" like the existing ones (they
  need a device handle the host never hands out). `hid::InitializeVibrationDevice` / `GetVibrationDeviceInfo` ran
  without a stop (`[hid] the game initialized its vibration devices`).
- **`nn::os::CreateThread` broke in the SDK** (`ThreadManager::CreateAliasStackUnsafe` failed 64 tries). With
  `TOTK_MEM_LOG` (it now also logs `svcMapMemory`): the stack source was `main+0x1da7000`, in main's `.bss`; the host
  accepted only heap and alias memory. On the console module data (CodeData) is a valid source. The host now accepts
  a range inside one module's data or `.bss` whose pages are all read-write and unlocked
  (`runtime_module_data_rw` in the loader); QueryMemory reports such a source as CodeData.

Verified: the headless boot now runs past account, performance mode, keyboard, vibration devices, AudioOut, four audio
renderers (REV8 update) and three thread creations with stacks from main's `.bss`, then stops at the VI display
service, which needs the native window (`VI requires native macOS window`).

A run with a window could not continue on 2026-10-25: the Mac's screen was locked and the display asleep
(`CGSSessionScreenIsLocked = 1`, no active display), so `CVDisplayLinkCreateWithActiveCGDisplays` returned -6661 and
the runtime stopped at `native display link initialization`. Not a defect of the port; the next run needs an unlocked,
awake display.

Commit: `7a8a428` feat(dread): answer the fs allocator, exit-request sections and nfp calls, and accept thread stacks
in module data.

### 2026-10-25: stage 3 done, the first frame is presented

With the screen unlocked, runs use a window and `TOTK_GPU_ALLOW_SUBMIT=1`, each capped at 90 s.

- **The VI display and NVN window come up** (2 swapchain images, 1600x900, format 0x25). The game then crashed in its own
  code (`main+0x1a0310`, `[x20+0x390]` with x20 = 0), the same place every run, so not a race. In lldb the lookup
  that returned null (`main+0xeab90`) was asking for `system/engine/prjdecals/shotimpact00_def.bsmat`. The asset is in
  `packs/system/system.pkg`: Mercury packs index files by CRC-64 of the path (reflected ECMA polynomial, initial value
  all ones, no final XOR), and that hash is in the pack's table of 272 entries.
- **The cause was the RomFS mount.** `TOTK_FS_LOG` lines for `rom:/` paths came from the host's save-path code, and no
  RomFS read was ever made: the game had not mounted its RomFS. At `main+0x132424..0x132458` it asks
  `QueryMountRomCacheSize`, allocates a buffer of that size and calls `MountRom` only when the allocation is not null.
  The host answered 0, so the allocation was null and the mount was skipped; every pack and asset open then failed
  and the game ran on with empty tables. `QueryMountRomCacheSize` now answers what the SDK computes: the four metadata
  tables of the verified image (directory and file hash tables and entries). TotK and Bayonetta 3 accept 0 and are
  not affected (other folders).
- New diagnostics, off by default: `TOTK_FS_LOG=1` logs the RomFS mount, `TOTK_FS_LOG=2` also every RomFS read.

Verified: RomFS mounted as `rom`, 771 RomFS opens, the save folder mounted, **"first presented frame" at 1.214 s**,
frames 1-3 presented (clears only: a loading screen) with 99 NVN functions in use.

**Next stop (stage 4): the title movie.** The game builds a `movie::Decoder` of type 3 (VP9), output format 3, mode 2.
The host's decoder (`movie_host.c`, from TotK) covers VP9 in buffer mode (mode 1, NV12) only, so this one goes to the
SDK's library, which needs Tegra's decoder and crashes in `android::Surface` (`subsdk0` `DecoderImpl::Configure`,
called from `main+0x225870`). Dread's movie player is `main+0x220e24..0x2268dc`. In mode 2 the game hands the decoder
NVN objects (`movie::Buffer::SetNvnDev` at `main+0x2267f0`, `SetNvnSync` at `main+0x226804`, after
`GetOutputBufferFormat` at `main+0x2267c4` and before `GetOutputBuffer` at `main+0x226834`). The decoder writes
frames into NVN memory and signals an NVN sync. That needs reading before the host decoder can serve it. The movies
are WebM; main also uses `movie::Extractor`, which runs in the library (WebM parsing, no hardware).

Commit: `f671685` fix(dread): QueryMountRomCacheSize answers the RomFS metadata size, so the game mounts its RomFS.

### 2026-10-25: stage 4 (in progress), the title movies

**Texture mode.** Dread's movie player (`main+0x220e24..0x2268dc`) builds `movie::Decoder` VP9 in mode 2 with
output format 3. For each frame it calls `GetOutputBufferFormat`, then `GetOutputBuffer` with a `movie::Buffer` built
by `SetNvnDev`, `SetNvnSync` and `SetDataAndCapacity(obj+0x18, 8)`; on success it does not release the index itself.
The library's name for this is `DecoderImpl::CopyOutputBufferToClientTextureAndRelease`. The buffer's data was first
read as a pointer to a texture; the logged value `0x100584554` has the low word `T E X 00`, the graphics layer's
texture magic, so the data *is* the `NVNtexture` object, embedded in the game's structure. The texture is
`RGBX8_SRGB` (0x37), 1280x720, from a pool of the game's textures matched by size (`main+0x224670`).
`movie_host.c` now serves this mode: NV12 is converted with the matrix and range the stream declares (it declares
none: BT.709 limited range, from 720 lines), written with the layer's own `nvnTextureWriteTexels`, the sync is fenced
with `nvnQueueFenceSync`, and the index is released. Verified: `titlescreen_ini.webm` decodes 431 frames at 0.75 ms
each with no errors, and screenshots (`TOTK_INPUT_FILE` `shot`) show the METROID and METROID DREAD title animation
with the expected colours. Commit `4f4bbb1`.

**Sound streams.** NintendoWare Atk opens `rom:/system/snd/../../sounds/streams/mainmenu_title_ini.dspadpcm.bfstm`.
The host refused `.` and `..` components; Atk aborted on a result it has no message for (`main+0x11dfcec`; its
table names only `fs::ResultPathNotFound`, `DataCorrupted`, `TargetLocked`). RomFS paths are now normalized as
Horizon's fs does (separators collapsed, `.` dropped, `..` resolved, never above the root); 21 synthetic cases in
`tests/test_dread_romfs_paths.c`. After this, audio plays: output level rms 0.10-0.14, no underruns. Commit `e4d6f00`.

**Shader tools.** `make shader-tools` must run once per work folder (prebuilt `totk_xlate`, SPIRV-Cross at the pinned
commit); without it the runtime fell back to a Python service that the work folder does not have.

**Movie event ordering.** When the title loop started, the movie thread crashed in `nn::os::WaitAny` on a linked holder
with a null vtable (finalized but still linked), in 2 of 3 runs, not always: a race. The host noted event signals under
the decoder's lock and fired them after it, also from the present thread, so a signal could reach an event the game
had just finalized when it stopped and destroyed the first decoder. Stop, Reset, RegisterClientEvents and the
destructor now revoke a decoder's pending signals (an epoch per decoder, a read-write lock around firing), as the
library guarantees. In 11 runs since, no `WaitAny` crash. Commit `6255ee6`.

**RomFS unmount.** The game unmounts `rom`; `runtime_romfs_owns` expects a path, so the host stopped with "unmount of
unknown fs mount". `runtime_romfs_unmount` releases the name. Commit `6255ee6`.

Open, not understood yet:

- 1 of 11 runs crashed in `nn::os::LockMutex` on a garbage mutex as the title loop opened: the movie thread received a
  message (`main+0x22376c`, `TimedReceiveMessageQueue`) whose object at `+0x30` was `0x7fffffff00000000`, i.e. a
  message about the first movie's player outlived it. Its sender is outside the movie player's code range and has not
  been found.

### 2026-10-25: the "quit after a button press" was the first save

The user confirmed pressing buttons on a connected Switch Pro Controller in the runs that quit. A scripted press
(`TOTK_INPUT_FILE` `key 38 150`, J = A) on the title loop reproduced it every time, and `hletrace 2` with the new
save-side result log (`TOTK_FS_LOG`) showed the sequence: the game opens `savedata://userdata/` (an empty path
component and a trailing separator), the host answered "invalid path" (`0x2ee602`) where Horizon normalizes the path
and answers "not found", so the game never created the folder, `CreateFile savedata:/userdata/userdata.bmssv` failed
(`0x202`), and the game shut down in order (`SetUserInactivityDetectionTimeExtended`, `time::Finalize`,
`account::CloseUser`, `EnterExitRequestHandlingSection`, `Unmount`, return from `nnMain`). Save paths now collapse
repeated and trailing separators; `.` and `..` stay invalid there because the result is a host path. Commit `b760a73`.

Verified: with the scripted press the game creates `userdata/userdata.bmssv` and `profile0/{common,pkprfl,samus}.bmssv`
and keeps running; a 70 s run without other input stays up and screenshots at 37, 47 and 59 s show the SAMUS FILES menu
(file 01 from the user's earlier session: 15/15 missiles, 99 energy, 00:00:00; files 02 and 03 empty) with the animated
background and the button prompts.

### 2026-10-26: starting a game; threads on Horizon's cores

Driving with scripted presses (title, SAMUS FILES, file 01, A every 4 s) starts the prologue: the movies
`prologue_01_2` ... `prologue_11` and the narrated pages (Metroid, X Parasite, B.S.L. station) show correctly. The first
area (`packs/maps/s010_cave`) has not been reached yet.

- **Start's signal.** `h_start` signalled "input available" inside `movie::Decoder::Start`; the game links the new
  decoder's wait holders on its worker after Start returns, so `SignalEvent` met an unlinked holder at the first movie
  switch. It is now delivered at the next present. Commit `3dbf60a`.
- **Cores.** Threads created with ideal core -2 ran on any of the three cores, and the main thread had TotK's priority
  44. Horizon pins a -2 thread to the process's default core (main.npdm: 0) and starts the main thread with the npdm
  priority (39) on that core; `SetThreadCoreMask` -2 is the default core, -1/-3 leave the ideal core. Commit
  `19c5908`. The movie worker is thread `0x100d`, priority 30, core 0, the same core as the main thread.
- **Preemption at wake.** The host handed a core to a woken higher-priority thread only at the holder's next SVC; Horizon
  does it as the waking SVC returns. Now the core is reserved at wake and given up at SVC exit. Title test, 5 runs each:
  eager (new) crashes exactly when the first movie ends naturally (3/3, 0/2 otherwise), lazy (`DREAD_LAZY_PREEMPT=1`)
  4/5. Commit `c54be95`.

Open, deterministic: **a movie that plays to its natural end leaks its wait holders.** `DREAD_OS_TRACE` shows the first
title movie's four holders (in its `DecoderEvents`, the decoder wrapper + 0x60) initialized by the main thread and
linked by the worker, and never unlinked before the wrapper is freed; the worker then walks freed memory in
`nn::os::WaitAny`. When the game stops a movie early (a press during it), the worker unlinks and finalizes them first.
What is known of the game's side:

- the worker (`main+0x222624`) links and unlinks a player's holders on requests queued by `main+0x222838`, which come
  from its message handler `main+0x21cf04`: type 5 links all (`0x224298`), type 11 unlinks all (`0x2243d8`, over the
  player's track list at +0x60/+0x68), types 3 and 6 do other work; a call with the second argument set is ignored;
- the player's state is at +0x2c; close (`0x21e5d0`) sets 11 only from 10 or 13 and notifies the observers at
  +0x720/+0x728; stop moves to 10 only from 6, 7 or 9;
- the end-of-stream handler (`0x21b5b0`, on an output buffer with flag 4) marks the wrapper finished, notifies with the
  ignore flag, and posts command 0 to the player's own queue (`0x21fd20`);
- at Stop after a natural end the player is in state 11 with 1 track, so the type-11 message either does not reach the
  worker's handler or is ignored there. Next step: breakpoints (or an observer trace) on `0x21cf04` and `0x2243d8` in
  that path. lldb works with `settings set platform.plugin.darwin.ignored-exceptions EXC_BAD_INSTRUCTION` and SIGILL
  passed through, but main's address under lldb was not where the breakpoints were placed.

Also seen: in two long runs the game stopped presenting frames for ~40 s with one thread at 100% after
`prologue_09_10` / `prologue_11` opened (with and without the eager preemption); not investigated yet.

### 2026-10-26: the first area

The open items of the previous entry were scheduling and two host costs; each was found with a thread dump
(`TOTK_INPUT_FILE` `threads`) and a host sample (`sample <pid>`) taken while the game was stuck.

- **The natural-end movie leak** (`bf80dfe`). lldb breakpoints on the game's player (set from the runtime's own
  `runtime_module_base("main")`; launch stopped, break on `h_configure`, then source the breakpoint file) showed the
  sequence: link-all (state 5), 6, end, 10, close, 11, **unlink-all runs and its filter matches**, the unlink request is
  queued, and the decoder is freed before the worker handles it. The waker had equal priority; on Horizon the woken
  worker is runnable at once and is chosen over the main thread when the waker blocks; the host only counted it once
  its host thread reached `core_take`. Every woken thread now reserves a core. With that alone the game stalled after the
  movie: after the end-of-stream input the host still offered input, and two priority-30 movie threads fed end of
  stream in a loop (5.5 million lock SVCs each), starving the main thread. No input is offered after end of stream.
  6 title runs, 4 with a natural end: no crash, no stall.
- **WriteTexels** (`dc7ebee`). At the area load the main thread spent tens of seconds in `write_texels`: each call on
  mirrored storage deswizzled and reswizzled the whole level. `nvn_swizzle_region` writes only the region;
  `tests/texel_region` (`make test`): 320 random regions identical to the old path, a broken copy fails.
- **GPU paths.** The area hit `TOTK_GPU_MIDCLEAR`'s isolation stop (mid-pass depth-stencil clear). The TotK and
  Bayonetta launchers enable `TOTK_GPU_WIDEN`, `BLOCKRT`, `MIDCLEAR` and a 5000 MB budget for normal play, and so does
  this fork's launcher; test runs now use the launcher's GPU settings.
- **Counter reads preempt** (`73e5a97`). Then the main thread held core 0 for 17 s polling the counter while a
  higher-priority thread (0x1009, 32) waited for the core. The timer gate is now a preemption point.

Verified: one 8.5-minute drive (title, file 01, prologue, A every 4 s) stays up, presents 19,200 frames, and screenshots
show the in-engine opening scene (Samus close-up) and the first Artaria room with HUD (15/15 missiles, 99 energy),
minimap and the first tutorial prompt ("While tilting L to run, press ZL to slide."). The first time through, the area's
shaders are translated and compiled; the cache in `build/shader-cache` holds them for the next run.

Not checked yet: walking around, sustained frame rate in the area, audio in the area, the rest of the prologue frames
(only screenshots), long play.

### 2026-10-26: playing, two leaks, and the Builder

- **GPU memory on the title loop** (`e6bb3f5`). The first sessions the user played stopped at the GPU budget (5000 MB)
  after about 40 s on the title screen: GPU memory grew by about 527 MB every 5 s, 150 movie frames x 3.5 MB.
  `mtl_write_texels` (and `mtl_read_texels`) allocate a staging buffer and an autoreleased command buffer that holds it;
  the host movie decoder calls WriteTexels from a game thread without an autorelease pool, so both lived until the
  thread ended (each movie switch, which is why scripted runs that left the title early never saw it). Both now drain
  their own pool. 75 s on the title: 1753 MB at frame 300, 1756 MB at frame 3300. MetalFX was not involved.
- The user then played a 5-minute session (title, menus, prologue) without a problem and reported that everything
  played fine.
- **The Builder** (`55a6cea`). `game.json`, `build.py` (from Bayonetta's: XCI or NSP of the base game, four checked
  steps, `--inspect`, refusals), a launcher without Bayonetta's settings, tests for both, and the Builder's own tests
  updated. `build_app.py <the XCI>` builds the app through the hand-off: 14 s with the work folder reused, 1 min 16 s
  from an empty work folder (main's rewrite byte-identical to the reference).

### 2026-10-26: flat walls and a black Samus were empty reflection maps

The user saw blurry or flat textures across the first area (the save station, for example). With the user's save loaded
at the save station, a pass dump showed the G-buffer: Samus and the door panels had their detail, the walls and
machinery were flat colour with no normal-map detail, and the lit picture had Samus as a black silhouette.

- The wall materials' own textures were right. Texture 819 is one colour in Metal, and its guest memory decodes to the
  same colour in every block (sRGB 74, 93, 106), with only alpha varying. No write was missed: `DM_NVN_DIRTYCHECK=1`
  reported 0, and `DM_NVN_TILELOG=512x512` showed one first upload per texture.
- Every material also binds two reflection cube maps (BC6H, 128 and 64 px, no packaged layout). Every face and level of
  both was black in Metal, while their guest memory held data (15,420 of 18,432 words non-zero). They sit in the same
  `CPU_NO_ACCESS | GPU_CACHED` pool as the packaged materials. `tex_mirrored` mirrors packaged textures and textures in
  CPU-visible pools only, and left everything else in a GPU-only pool GPU-owned, so the cube maps were created empty.
- **Fix:** a block-compressed texture in a GPU-only pool mirrors its pool bytes. It cannot be rendered into, so the pool
  bytes are its contents. Render targets, depth, display and compressible textures there stay GPU-owned as before.

Verified: same save, same room. Samus is lit in her suit colours, and walls and machinery have reflections and surface
detail. No new errors in the log; memory 2,888 MB vs 2,883 MB before. Not checked: other areas, the title-screen visor
(it looked blown out before this change).

### 2026-10-26: the main thread's priority from main.npdm

`prepare_runtime.py` took the main thread's priority from byte 0x0c of the META header and the default core from 0x0d.
Those are the process flags (0x27) and a reserved byte; the priority is at 0x0e (44) and the core at 0x0f (0). So the
main thread ran at priority 39 instead of 44 (the core was 0 either way). It now reads 0x0e and 0x0f and checks that
the priority is below 64 and the core at most 3 (`017c2cc`). Verified: `runtime_tables.h` has `NPDM_MAIN_PRIORITY 44`;
the title, the save menu and the first area still play.

### 2026-11-01: iPhone / iPad

**Goal.** The game on an iPhone 16 Pro Max (iOS 26.6.1, 8 GB), built from the same work folder as the Mac app.

**What iOS needs that the Mac does not.** Code written into memory at run time cannot execute, so the rewritten modules
are compiled into the app (Tears of the Kingdom's method: `loader.c export-ios` writes the text as the game patches leave
it, `ios/tools/gen_embed.py` puts text and patch area into the r-x segment `__TOTKX` and the rest of the file into a rw
segment per module, and the iOS `map_module` `vm_remap`s them back into one reservation, checking each part's SHA-256).
That path refused the leading patch area. Now:

- `map_module` (iOS) reserves `patch_lead + image` as on the Mac, remaps the patch area (`x1`, the file's first 4 MiB)
  r-x before the module base, the text r-x at the base and the rest of the file (rodata and data, `w`) copy-on-write
  after it. `.bss` and the key page stay the reservation's zero fill, so main's 1.8 GiB `.bss` is not in the app.
- `gen_embed.py` (Dread's copy) reads `PATCH_LEAD` from `fixups.tsv`: `x0` is the file from `PATCH_LEAD` on, `x1` the
  patch area at its start.
- `export_ios_text` read the "original" text from the start of the file, which with a leading patch area is the patch
  area: it now seeks to `PATCH_LEAD`. Dread has no game patches, so the export reports `patched_words=0` for all three
  modules (and so equals the verified image).
- `movie_host.c` included `<mach/mach_vm.h>`, an `#error` in the iOS SDK: it uses `mach_vm_compat.h` like the other
  files. `frontend_ios.m` gains `runtime_shortcut_help_toggle` (a no-op: the shortcut list is Mac only).

**The app** (`ios/`): `Sources/main.m` is a full-screen Metal view, the Mac launcher's player environment with every path
in the app (`GameData/romfs`) or its Documents (profile, game slot, RomFS proof, shader cache, `dread.log`), and the
loader's `main` (built as `dread_host_main`) on a 64 MiB game thread; no touch controls or settings pane yet. The device
cannot translate shaders, so the app carries the Mac's translations (`build/shader-cache/rt-<chain>`) and copies them into
its cache at start (`TOTK_SHADER_SEED=off`: there is no prepared seed). `Dread.entitlements` asks for the increased memory
limit. `tools/build_ipa.py` writes an Xcode project into the work folder (absolute paths to this folder's sources and the
work folder's tables) and builds with xcodegen / xcodebuild; Opus and libvpx are built for the device
(`build_deps.py`, a copy of TotK's). `nvn_bctc.metal` is left out: the runtime compiles its copy from source
(`k_bctc_msl`), and including it made the build need the separately downloaded Metal toolchain. `install_device.py` is
TotK's without its game-data and shader-seed pushes. `build.py --platform ios`
runs the four steps, compiles the Mac runtime (export-ios runs it) and calls `build_ipa.py`; it warns when the Mac has
translated no shaders.

**On the way.** `xcodebuild` needs `DEVELOPER_DIR` when `xcode-select` points at the command line tools
(`xcode_dir.py` finds `/Applications/Xcode.app`). A pyenv `ninja` shim that names no installed version breaks CMake's
Ninja generator; Homebrew's `ninja` first in `PATH` fixes it. The Mac had the player's Apple Development certificate but
not Apple's WWDR G3 intermediate (only the expired 2023 one), so `find-identity -v` listed no valid identity until G3
(from apple.com/certificateauthority, chained to Apple Root CA) was added to the login keychain. The Simulator build
(`build_ipa.py --simulator`) stops at MetalFX, which the Simulator SDK does not have; not pursued.

**Verified.** Mac: `make probe` passes after the loader change; `python3.14 -m unittest discover -s tests` passes. The app's executable has `__TOTKX` r-x and
`__TOTK{main,subsdk0,sdk}W` rw. `build.py <xci> -k prod.keys --platform ios` reuses the verified steps and writes a
4.2 GB IPA in 25 s. On the iPhone (free personal team, signed for 7 days): `mapped embedded main at 0x300400000
size=1922859008`, guest alias region 16 GiB, the RomFS hashed once (9,654 files), 1,141 shader files imported. The title
movie runs at 60 fps (frame time p50 16.67 ms, p95 16.9 ms, no dropped frames over 1,800 frames; GPU 1.2 ms per frame),
audio without underruns. The player then played for about 8.5 minutes (30,300 frames) and reported no problems; peak
footprint 2,678 MiB, no `[shader-miss]` in either run's log (everything played was in the Mac's cache). A few single
frames took about 1.1 s (frame 9045, 9661: area loads, as the long-frame lines show no shader or pipeline work).

Not checked: an iPad; areas the Mac has not played (their draws would be skipped); a phone with less memory.

### 2026-11-01: touch controls on iPhone / iPad

`ios/Sources/touch_controls.m` is TotK's on-screen controller (`games/totk/ios/Sources/touch_pad.m`) without its
frame-rate button and Settings-pane switches: the Switch layout (ZL/L, −/+, R/ZR along the top; d-pad and left stick
on the left, A/B/X/Y and right stick on the right; L3 / R3 in the bottom corners), so the game's own prompts match. A
touch that misses every button is a stick centred where it went down. It feeds `runtime_touch_pad_set`, which
`runtime/gamepad.m` already merged with physical controllers (buttons OR-ed, the stronger axis wins). It hides while a
controller is connected and comes back on a touch.

Verified: on the iPhone 16 Pro Max with the controller disconnected, the player reported menus, movement and
multi-finger moves (holding L to aim while moving, shooting while moving) all working. Not checked: an iPad.

### 2026-11-02: a prepared shader seed for iPhone / iPad

The device cannot translate shaders, so until now it drew only what the Mac had translated while playing. The iOS build
now prepares every shader of the game from the player's own RomFS (`scripts/prepare_shader_seed.py`, forked from TotK's
for this folder's older runtime):

- **Where the shaders are.** All of them sit in `system/shd/*.bshdat`, MercurySteam's MSHD files: 478 files, 1,342 NVN
  control sections, 1,010 unique shaders (671 vertex, 671 fragment programs). Each control section (magic `0x98761234`,
  0x880 bytes, hashed into the key with the code as `nvn_mtl.m prog_get` does) is followed by the shader's assembly text
  and then its code (magic `0x12345678`); the first 4-aligned code header after a section is its code. A scan of every
  other RomFS file and the three game modules found no other control section.
- **What is translated.** Each shader for the default bind state, then every state `assets/shader_list.tsv` names, named
  and matched as `totk_shader_rt.m` does (variant = key + SHA-256 of the answers' sorted JSON; `deps_match` without
  wildcards, unanswered slots 2d / float). Vertex shaders are translated with `depth_minus_w` (Dread sets NEAR_IS_MINUS_W:
  the first try without it gave different MSL than the Mac's). One translator service per core: 1,044 variants in about
  2 s; a rebuild with the same inputs reuses the seed, a longer list translates only its new rows.
- **The list.** `assets/shader_list.tsv` (`scripts/shader_list.py`, TotK's with Dread's header) holds the 34 bind states
  of the Mac's 190 that the defaults do not serve: cube maps (57 slots) and depth textures (4). Identifiers only.
- **The seed** holds only what `totk_shader_seed.h` imports (`<key>.variants`, `<variant>_b.metal` / `_b.json`,
  `pso_manifest.tsv`; TotK's newer `shader-keys.json` would make this runtime refuse the whole seed).
- **The app** carries it in `GameData/shader-seed` (`build_ipa.py`; `build.py` runs the step), `main.m` sets
  `TOTK_SHADER_SEED_DIR` (it was `TOTK_SHADER_SEED=off`) and `TOTK_SHADER_CONTRIB` (a miss becomes a list row in
  Documents/`shader-list-contribution.txt`). The Mac's own translations still come along, with their pipelines; their
  import now adds the rows a `.variants` index lacks instead of replacing it, which would have dropped the seed's rows.

Verified: for all 190 shaders the Mac had translated, the seed's translation of the same bind state is byte-identical
MSL. Phone conditions on the Mac (an empty cache filled from the seed only, `TOTK_SHADER_NO_TRANSLATE=1`, a fresh
profile, A every 4 s from the title for 5.5 minutes): the seed imported (1,044 variants, 3,099 files), all compiled
(5.2 s on the first start, 0.14 s once Metal had them), 7,200 frames, zero `[shader-miss]`, and screenshots show the
in-engine prologue (the ship, Samus close-up with subtitles). The iOS build with the seed builds (1 min with the work
folder ready). `tests/test_dread_shader_seed.py`; full suite 1,302 tests pass.

On the iPhone 16 Pro Max (installed over the cable, a fresh Documents shader cache): the app imported the seed (1,044
variants, 3,096 files) 11 s after launch, the first frame came at 5.6 s, the startup pipelines were ready after 13 ms of
waiting and the rest compiled in the background (1,233 shader libraries, 221 pipelines in 57 s on one thread) while the
title and the movies played. The player then played for about half an hour (7.1 minutes of it with the app in front,
25,685 display beats and 21,600 presented frames over 12 trips to the background) and reported no errors; footprint
about 2.6 GB. The log has one miss: a fragment shader (`822cb9fd…`) with cube maps in slots 2, 12 and 13, needed around
frames 20,100 to 21,000 (about 3,800 draws skipped). Its row, from the app's `shader-list-contribution.txt`, is now in
`assets/shader_list.tsv` (35 rows; `shader_list.py prune` keeps it): the rebuilt seed holds 1,045 variants, and the new
variant's dependencies match the missed state as `deps_match` checks it. Not yet replayed on the phone.

Not checked: the first-start compile on an older device (`.metallib` built on the Mac would avoid it but needs Xcode's
Metal Toolchain), an iPad, areas beyond those played, and whether more bind states than cube and depth textures turn up. The Mac app is unchanged: its launcher does not set
`TOTK_SHADER_SEED_DIR`, and the Mac build does not prepare the seed (a work folder that has one, after an iOS build,
imports it on the next Mac start, as the runtime always did with `build/shader-seed`).

