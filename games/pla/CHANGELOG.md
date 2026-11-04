
## Stability — the periodic "RomFS read failure" crash is gone

Sessions ended at random with `[totk] FAIL: RomFS read failure`. The added diagnostic named one: reading
`bin/appli/tips/bin/tips_img_090_02.bntx` (2.28 MB) returned `EFAULT` (errno 14) with `done=0` — `pread` into a
guest heap address that guest code had never touched, so the page was never mapped in our address space; `pread`
checks the destination instead of faulting it in. The read path now maps holes over the destination range
(`mach_vm_region` + `mach_vm_allocate`) before reading, and falls back to a host buffer plus a copy through the
guest write path. Long main-story play after the fix: no crash.

## Rendering — texture uploads no longer race the guest's writes

The GPU deswizzle kernel read the guest pool when the command buffer executed, so a guest write that landed after
encoding could be read instead of the snapshot: roughly one launch in three came up with a half-lit face and/or
hard-edged black patches on the ground, and stayed that way for the whole session. Pre-swizzled textures now take
the CPU row copy by default (frame time unchanged: 33.33 ms, GPU 2.6 ms); `TOTK_DESWZ_GPU=1` restores the GPU path.


## Performance, round 2 — the 62 ms frame was the virtual core scheduler (A3)

Frames sat at 62.1 ms with 5 ms of CPU and 3 ms of GPU. Per-frame accounting (added here, behind the existing
diagnostics) put 62 ms of it in one place: the main thread waited a task condition variable ~24 times a frame, ~2.5 ms
each, zero timeouts, 5 us wake latency, 0.02 ms waiting for an emulated core. The three emulated cores (NCORE=3) were
being handed between ten-odd guest threads, serialising the game's task graph; `TOTK_FREE_CORES=1` showed the same
scene at **16.66 ms/frame, presented 16.67 ms** (45 s: 2598 frames, 2 long frames). The host scheduler is the default
now (`TOTK_VIRTUAL_CORES=1` restores the emulation).

## Performance, round 3 — native 60 fps is opt-in again

With the frame step at the measured 16.7 ms interval the game ran twice as fast (30 fps track is correct, checked
in game): part of PLA advances once per update instead of by dt, so doubling the update rate doubles that part. The
30 fps track is the default (speed right, key 2 gives the 60 Hz picture through 30+30 interpolation); the native
60 fps track is `PLA_REALTIME=1` (the launcher's 60 fps choice) until the per-update logic is found.

The ground shadow patches and the half-lit face seen in gameplay were fixed by the same pass work and are gone.

# Pokémon Legends: Arceus port — work log

Found / done / why / how verified / pitfalls, newest last. "Verified" only ever means a recorded check.

## Stage 0 — survey and originality (2026-10-31)

**Inputs.** Card image `Pokemon Legends Arceus [01001F5010DFA000] [v1.1.1] (1G+1U) (MOD11.0.0).xci` (6.1 GB):
base Program NCA `63d25921…` (1.0.0, 6,459,375,616 B) + update Program NCA `b38cd4c1…` (1.1.1, 91,726,336 B,
BKTR). Note the update Program NCA's header title ID is the **base** ID (`01001F5010DFA000`); the update title
`01001F5010DFA800` appears on the update's Meta NCA. First `--update-title 01001F5010DFB000` guess failed the
mixed-title check — the real update title is base+0x800.

**Fork.** `games/by3` at `feat/nier-latest` `c6fee464` (white-platinum-era SDK line, GPU-indirect and staged
program-binding fixes), with the three RomFS scripts from `games/alc/scripts` (`feat/alc-1.0.1` `e569c0ac`)
because our input is a card image, not base+update NSPs. Renames applied repo-wide (`by3`→`pla`, `BY3_`→`PLA_`,
`v120`→`v1101`, `GAME_PLA` in `game_version.h`, `runtime_pla_override`); zero leftover references (grep).
Sources: `third_party/COPIES.md`.

**ExeFS.** `extract_exefs.py` (alc card variant, A1-parameterized) extracted base and update; FS header,
hash table and every data block verified. Update main 31,882,976 B, build ID
`AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E0`. Pinned in `assets/expected-v1.1.1.json`. No community cheat table
was found to cross-check the build ID (searched the game-drive for `AEE8F150`/`01001F5010DFA000` directories);
originality rests on the block hashes and IVFC levels.

**npdm.** 28 services in the ACI0 whitelist: `acc:u aoc: audin: audout: audren: bcat: bsd: caps:s caps: ectx:a
friend: htc:ten ldn: ldr:r mii: mm: nfc:mf: nfc:use nfp:use ngct: nifm: nim:ec notif: nsd: pl: prepo: time: vi:`.
Nine of them are online-stack services — this game carries NPLN (see below).

**Middleware / engine strings** (from the decompressed update `main`, 70.3 MB memory image):
GameFreak's own `gfl` engine (project `hayabusa`, built on Windows Jenkins — `C:/jenkins/workspace/hayabusa/...`
paths in sol2 glue lambdas), sol2 + `.blua` Lua scripts, `bin/haxe/` scripts, Havok physics (`.hkx`, 80 files),
gRPC + protobuf + `nn.npln.friends.v1` + webrtc (the online stack is linked in), `nn::codec::HardwareOpusDecoder`
(hwopus). Audio is **Wwise** (`bin/sound/NX64/wwise/`: `Default.pck` 674 MB, `Common.bnk` 15 MB, `Init.bnk`,
`AK::BankManager/EventManager/IOThread/OpusDecodingThread/SpatialAudio` strings in main) — Wwise's Opus thread
is why hwopus is imported. **No video middleware** (no Bink/Sofdec/.usm anywhere): `bin/demo/` is in-engine, so
`movie_host.c` should stay dormant.

**NVN import diff.** 524 `nvn*` symbols in main vs the 523-entry graphics-layer table: only
`nvnBootstrapLoader` is missing — the same gap astral chain had, so the existing host path should hold.

**RomFS.** Base: all six IVFC levels verified, 18,370 files, 6,400,769,560 B, sha256 `9688c20c…`. Merged
(base+update BKTR): all six levels verified, 19,095 files, 6,421,372,960 B, sha256 `73a7369b…`
(bytes from base 6,388,383,776; from update 45,621,248 — the patch touches a sliver). Content: `.bntx` 2,969,
`.gfpak` 678, `.trmsh/.trmdl/.trskl/.tranm/.tranm` (gfl formats), `.ptcl`, `.hkx`, `.wazabin` (move scripts).

**Pitfall found and fixed — the PLA variant of A4.** `merge_romfs.py` failed at **IVFC level 4** with
`Base RomFS read out of bounds` (astral chain's A4 was level 5). Probe on the update's indirect table: PLA's
update recomputes **only the hash-tree blocks it touches** — row `virt 0x14020..0x658a0 -> base @0x14020`
targets the base section's own level-4 hash-tree region, i.e. a storage-0 target **below** the base level-5
offset (0xc04000). `romfs.bin` holds only the level-5 data layer, so subtracting the level-5 offset went
negative. Fix in `scripts/merge_romfs.py` (`BaseImage`): storage-0 targets at/above the level-5 offset index
`romfs.bin` as before; below it they read the base section straight from the card via its own AES-CTR key
(`BaseImage.read` branch + `bctr` in `main()`). Verified by the merged six-level pass above. To write back to
the skill's A4 page as a variant.

**Registered but not yet exercised:** `make prepare` needs a rewrite first (stage 1); `rewritten_sha256` in the
expected-json is empty on purpose.

## Stage 1 — code rewrite (2026-10-31)

`patch_nso.py` rewrote main, sdk and subsdk0 (`DM_RULESET=native DM_PRIVATE_GATES=0`): main 34,940 sites
(16,528 x18, 18,250 tpidr reads, 155 svc gates, 2 jump-table spsets), sdk 12,085, subsdk0 10,486. PATCH_OFF
0x4360000 (~70.4 MB) — the append layout fits, the ±128 MB branch-range two-file layout of A2 is not needed.
Verified: `structck.py` OK for all three modules (0 violations); `equiv.py` OK — main 34,785 sites / 0
divergent, sdk 11,975 / 0, subsdk0 10,486 / 0; 600 of 600 mutations caught per module (seed 2); second rewrite
byte-identical (SHA-256 in `assets/expected-v1.1.1.json`).

**Verifier enhancement** (`third_party/d2r/equiv.py`): the first two runs missed mutations — a `prfm pldl1keep,
[x18]` site (prefetch has no architecturally visible effect, so nop-mutating it is a meaningless negative
control) and bit-pattern-sensitive sites (`orr x18,x18,#0x20` under random initial values; `csel
x18,x0,x18,hi` when the condition holds — both sides pick x18 itself). The mutation sample now excludes
side-effect-free instructions, and mutation trials run three targeted states (x18=0, x18=all-ones, nzcv=0) on
top of the random ones. With seed 2: 600/600 on every module.

## Stage 2 — boot to nnMain (2026-10-31, in progress)

Five host fixes so far, in boot order:

1. **Out-of-image RELATIVE addend** (`runtime/loader.c`): PLA main carries one RELATIVE relocation whose addend
   already points past the image in the original NSO (`main+0x42722a8 -> +0x461a488`; gfl data the game itself
   overwrites during init — on the Switch it lands in the heap mapped right after main). The loader now grows
   the module mapping to hold the largest such addend as zero-filled memory behind the image, and relocate()
   accepts an addend inside the image or inside that room. `scripts/prepare_runtime.py` accepts the same
   addend only when it is byte-identical to the original's.
2. **The VAMM region does not fit below 2^39 on macOS 27** (`runtime/services.c`): PLA's `nninitStartup` carves
   one `AllocateAddressRegion(32 GiB)` out of the region GetInfo(2/3) reports. macOS 27's shared cache covers
   everything up to 0xF_C000_0000 and the system drops a ~24 GiB reservation at a random place inside the 64 GiB
   window at 0x70_0000_0000, so no 38 GiB free hole exists there (measured over several boots; the shrink-to-fit
   path ended at 12 GiB and the allocation failed with 0x1803). The VAMM region is real RAM on the Switch (the
   VammManager keeps books in user mode; AllocateMemoryPages only tags attributes), so it is now hosted as a
   plain lazy-commit mapping at 1 TB (`VAMM_SPACE_AT`), 64 GiB, with `svcMapPhysicalMemory`/`Unmap` implemented
   as mprotect over a per-page bitmap (`vamm_map`).
3. **Two pools, not one** (`runtime/services.c`): PLA plans its commits from the pool GetInfo(6/21) reports —
   it submitted 5.9 GiB against a 6 GiB pool and 2.95 GiB against the retail 3.2 GiB one, always leaving itself
   just over the physical budget, which then failed with LimitReached. GetInfo now reports the retail pool to
   the game while the physical-budget check uses a wide 6 GiB value (`APP_POOL_CHECK`); macOS anonymous memory
   is lazily committed, so the booked 3.1 GiB is resident only in the fraction the game actually touches.
4. **Thread priority range**: PLA's main logic thread is created with priority 16; the host's CreateThread
   check (totk's 28..59) rejected it, and the SDK asserts on the svc error instead of returning it. The check
   now accepts the full Horizon user range 0..63 (`runtime/threads.c`).
5. **sdk's assert PLT slot** (`runtime/loader.c`): `ThreadManager::CreateThread`'s diagnostics go through a PLT
   slot (sdk+0xc65678) that neither RELA/RELR nor the in-guest rtld fills — the GOT keeps raw file bytes
   (0x56b5c0, the lazy-PLT stub chain, and PLTGOT[0..2] stay zero), while the retail rtld fills it at module
   link. The host points it at sdk's own `AbortImpl`.

With these, the boot reaches **nnMain**: all module constructors run (main 1,895, subsdk0 44, sdk 115), RomFS
(19,095 files) is verified and mounted, and the game's early init allocates from the VAMM region.

**Where it stands.** The first `nn::os::CreateThread(prio=16)` still dies in an SDK assert: after our
AbortImpl fill the chain is CreateThread → `nn::diag::detail::AbortImpl` → Break, i.e. ThreadManager::CreateThread
itself asserts for another reason, and separately the guest-side rtld writes garbage over large parts of the
sdk PLTGOT (whole slots read back as 0xf85f03f017d3838x — contiguous, address-dependent values, written by
guest code between seal and nnMain; the same mechanism on totk's SDK 13.4 works). Both are the next stage-2
targets. Diagnostics: `PLA_SKIP_SDK_BREAK=1` continues past non-fatal SDK asserts (diagnostic only); the Break
handler dumps the message buffer, the UserExceptionContext dying-message slot and the argument registers.

## Stage 3 — first frame campaign (overnight 2026-10-31)

Windowed boot (`make boot`, no TOTK_HEADLESS) with `TOTK_FREE_CORES=1 TOTK_GPU_MIDCLEAR=1 TOTK_GPU_ALLOW_SUBMIT=1`.
Fixed, in the order the game hit them:

1. **G2 resurfaced in our own Makefile**: `TOTK_HEADLESS=$${TOTK_HEADLESS-}` put an empty-but-present variable in
   the environment, which the presence-based check in loader reads as headless — windows never opened. The prefix
   is gone (headless stays explicit `=1`, windowed is the default).
2. **NVN unimplemented stubs, each dying loudly at first call**: `nvnTexturePoolGetSize` /
   `nvnSamplerPoolGetSize` (the descriptor capacity stored at Initialize), `nvnQueueBuilderSetQueuePriority`
   (accepted; the host keeps native macOS scheduling — the builder struct had no room for the field, so it is
   not stored), `nvnBufferBuilderGetSize` (the size stored at SetStorage). After these, zero unimplemented NVN
   calls remain in a normal boot.
3. **hwopus IPC service** (`runtime/hwopus.c`): PLA's Wwise threads decode their streams through the real
   IHardwareOpusDecoderManager / IHardwareOpusDecoder commands (Open with rate/channels + work buffer, then
   Decode with Nintendo's 8-byte big-endian frame header). Hosted with libopus; integrity check logs but does
   not die.
4. **pl:u shared fonts** (`runtime/pl_u.c`, alc's implementation, plus alc's `shared_font.c` whose
   `GetSharedFont` override replaces the whole IPC dance with real 64-bit pointers): firmware 22.5.0's six
   system BFTTFs are extracted by `scripts/extract_shared_fonts.py` and verified at load (17.4 MB).
5. **Thread stacks in the VAMM region** (`runtime/services.c` stack_move): PLA's thread stacks are committed
   inside the 32 GiB carveout, and `nn::os::CreateAliasStackUnsafe` maps them into the alias region with
   MapMemory — a source the TotK-era check never saw. Same treatment as an alias source: pages must be
   committed, mapping moves them (source goes PROT_NONE, VAMM bitmap uncommitted), unmapping restores them.
   This unblocked `nn::os::CreateThread` (the first two worker threads now come up).
6. **fs state plumbing**: PLA's per-thread `FsContext` accessors, `SetAllocator` and the Uid form of
   `IsSaveDataExisting` are answered by name (`runtime/fs_host.c`) so the SDK's real fsp-srv client never runs.

Where it stands: the window opens, Metal (Apple M5) initializes, the VI display comes up, worker threads run,
audio decoders get hwopus sessions, shared fonts load, and the game starts building its font/text rendering —
where it now faults reading `NULL+0x38` in its glyph pipeline (main+0x11f890 reads a flag at `[x8+0x38]` after
`main+0x123270` returned zero; `main+0x123270 → 0x121cb4` is the glyph lookup of a manager whose table is
empty). Per the iron rule this is an earlier init's symptom: the manager that should have been filled when the
fonts were registered did not get its table — next step is to trace where the game expects its font manager to
have been populated (GetSharedFontAddress/Size overrides return valid data; the BFTTF parsing happens on the
game side). Diagnostics left in: `PLA_SKIP_SDK_BREAK=1` (continue past SDK asserts), `TOTK_CRASH_MEM=<reg>`
(register-pointed memory dump at faults), the PLTGOT shadow check in `runtime_dispatch` (svc-granularity
change log), and the MapMemory parameter print in `stack_move`.

## Glyph-pipeline fault — evidence collected (overnight session, continued)

The fault at `main+0x11f890` (read of `[x0+0x38]` after `main+0x123270` returned zero) survives with richer
state. What is known now:

- The failing thread is the main thread, deep in the game's first-frame UI text render (13 worker threads,
  the window is visible at frame 0, the swapchain is up at 1920x1080, audio sessions exist).
- Chain: `nnMain+0x7c → 0x2781304 → 0x277c6bc → 0x277c7f0 → 0xfb5a74` (a draw-text helper with five call
  sites) `→ 0x2974fc0(obj, w1, 0x20, 0x3f) → 0x11f764/0x11f85c (fills a draw record from an info struct)
  → 0x123270 → 0x121cb4 (glyph-table lookup)`. The lookup reads `[this+idx+0x1888]` and
  `[this+idx*8+0x18a8]`; both are zero — the manager at `main+0x4176250` (BSS, reached via
  `[x22+0x60]+0x17340` from the render object at `[x26]`, vtable `main+0x417eed8`) has an empty table.
- The render object comes from `0xfb9604`, which takes a branch on a global flag byte at `main+0x4279520`
  (`ldarb`; zero takes the alternate path at `0xfb96f0`). Static scan found no direct writer of that byte in
  main — it is likely initialized indirectly (RELR-era data, another module, or a register-computed store).
- The game performs **zero file reads** before the fault (`TOTK_FS_LOG=1` prints nothing; `MountRom` is never
  called, so `rom:/` is not mounted), and the pl:u session only ever issues `RequestLoad` (command 0) —
  neither `Load` (command 3, which would copy font bytes) nor `GetSharedFontAddress` (the by-name override)
  is ever reached. So the game's own font data (RomFS `bin/font/*.BFOTF`, `font_24_bold.bffnt`) has not been
  read at fault time, and the pl shared-memory flow is not the data source either.
- Bookkeeping note: the MapPhysicalMemory "remaining" print used the retail pool and showed a wrapped value;
  it now prints against the wide check pool.

Next-session leads, in order of expected yield: (1) who initializes the manager at `main+0x4176250` — set a
host-side write watch on `main+0x4177ad8` (the table's head) at seal time to catch the initializer, or its
absence; (2) the alternate path at `0xfb96f0` and the writer of the `0x4279520` flag; (3) whether the game
expects an early resource-load pass (its own pak/archive reader) that never ran because something else
earlier in `nnMain`'s init silently short-circuited.

## Thread-state evidence at the glyph fault (same session, continued)

The crash handler now dumps every guest thread's state (`runtime_dump_threads()` from the SEGV path). At the
glyph fault: 22 threads exist; the main thread (0x1001, prio 44) is healthy and has made 1,395 svc calls
(latest SignalToAddress — it is running the task dispatcher); twenty workers sit in `WaitForAddress` (svc
0x1c) at their task-queue futexes as designed; and the faulting thread is **0x1017** (prio 28, seven svc
calls, last one SleepThread) — a render worker that went straight into text rendering instead of waiting for
its input. The glyph-table shadow check across all svc boundaries shows the table at `main+0x4177ad8` is
**never written by any guest code** through the entire run, and the PLTGOT shadow never fires either
(0xc65678 keeps its rtld-filled value — confirming the earlier "garbled GOT" readings were an artifact of a
wrong base address, G9).

Reading: the render worker races ahead of the game's own font initialization. On retail, the table it reads
is populated before any render worker touches it (either the worker is gated on a "fonts ready" event that
never fires here, or the populating pass runs inside code that took a different branch on this host). Next
steps: identify the creator of thread 0x1017 (prio 28 — one of the two prio-28 threads created after the
audio/graphic pools) and what event it should wait on; and find the writer of the manager table at
`main+0x4176250+0x1888` on a reference run (emulator or another title) to see what game code is supposed to
fill it.

## pl:u flow traced (same session, continued)

Instrumenting every pl:u command and the `GetSharedFontLoadState` override pins the game's font flow:
`RequestLoad` (command 0) is issued for font types 4/2/1/3, then the by-name `GetSharedFontLoadState`
override answers `Loaded` a handful of times — and the game **never** asks for the font data: no
`GetSharedFontAddress` (override), no pl:u `Load`/`GetSharedMemoryAddress`/`GetSharedFont` (commands 2-6).
Meanwhile the pl-internal manager slot at `main+0x4279520` stays **zero for the entire run** (svc-granular
shadow), so the render path at `0xfb9604` branches through the "not ready" arm with a NULL manager, and the
glyph tables at `main+0x4196250+0x1888` (pointed at via `[x22+0x60]+0x17340`) are never populated. The SDK's
lazy pl initialization therefore stops partway on this host — the session opens and RequestLoad goes out, but
whatever should construct the shared-font manager does not run. Next session: disassemble the SDK's pl lazy
init (the code between the sm: `pl:u` open and the first RequestLoad) to find which host answer it consumed
or missed — likely a GetInfo id, a memory-permission call, or an event the SDK waits on before building the
manager.

## Glyph lookup: the manager table is NOT empty (same session, corrected)

A 0x2000-byte FOLLOW dump of the manager (follow word 2 of the draw record = `[x19+0x10]`) corrects the
"empty table" reading. The manager at `main+0x4196250` is populated: `+0x1888` holds flag bytes
(`02 02 02 03 …`), `+0x18a8..+0x18c8` hold eight pointers (`0x10045bf5ce8` increasing by 2), `+0x19a8` holds
an index sequence `00 01 02 … 0f`, and `+0x1c30` points at the character→glyph map. The lookup in
`main+0x121cb4` walks a 184-entry stream (`w27=0xb8`) reading bytes from `+0x19a8`, mapping each through
`[manager+0x1c30][byte]`, and comparing against the requested character code (0x3f = `?`). It finds no match,
so `main+0x123270` returns zero and the caller dereferences it. In other words: the game is rendering a
string containing `?` through the *system* font mapping, and the character-to-glyph map has no entry for it —
the map is built from pl shared-font data that the SDK's half-initialized pl state never delivered.

Combined with the pl:u trace (RequestLoad ×4 → GetLoadState `Loaded` via the override → nothing else), the
picture is: the game's text system asked "is the font loaded", was told yes, tried to map a character through
system-font tables that were never imported (the SDK's own GetSharedFontAddress path would have imported
them), and hit the empty mapping. Next session: dump `[manager+0x1c30]`'s map and the 184-entry stream at
`+0x19a8` on a working run (or an emulator) to see what the map should contain; then find what SDK code
builds it — likely `nn::pl::detail`'s shared-memory import, which needs the Load (command 3) flow the game
never issues because it believes Loaded means "nothing left to do".

## Font gate + IPC round (same session, continued)

The glyph fault is beaten with a sanctioned **entry-form cut** (SVC_HOST_GLYPH_LOOKUP, first cut on this
fork): `main+0x123270` — the glyph-object lookup entry — is wholly implemented host-side (svc #0x82; ret,
registered in `third_party/host/hle.h`, served from `runtime/services.c` case 0x82 with a per-thread
writable zero block). The game's first-frame text render now takes the miss path 75 times per boot and
continues. Verified: structck OK (已登记剪口=1), equiv OK (600/600 mutations, cut sites excluded by design).
Three cut-mechanics lessons landed the hard way: the svc encoding must keep bit 0 set (0xd4001041 for #0x82 —
0xd4001040 is an invalid instruction and SIGILLs), the hle.h registry needs BOTH the `enum { SVC_HOST_X = N };`
row (number lookup) and the `// N SVC_HOST_X <main offset>（…入口…）` row (offset/form lookup), and the CUTS
key is the first 16 bytes of the build ID in hex.

Beyond it the game now reaches `persistent local account initialized` — with three new IPC services
(`runtime/hid.c` minimal hid stub, `runtime/csrng.c` with both CMIF and TIPC GetRandomBytes) and the E3-class
override `nn::account::OpenPreselectedUser` (non-Try variant, nn::Result).

Where it stands now: a new fault right after account init — a lazy singleton at `main+0x42f1798` is zero and
`main+0x2989bf0+0x394` dereferences it through the TLS cache path (`mrs tpidr_el0; ldr [x9, x0=0x200000]`).
Static scan found no writer of that slot, so its constructor runs in code not yet identified. Next session:
dump the faulting thread's guest stack to identify the subsystem, then find its initializer.

## Main loop reached, load loop stalls (same session, continued)

After the friends/GetFriendList override, the game's main thread runs its init to completion and enters the
**main loop**: the condvar trace shows it cycling through a task-queue (keys advance +0x2e0 per node:
0x100449601a4 → 0484 → 0764 → 0a44 → … → 0x44961b84+), with worker 0x1005 processing nodes in lockstep
(nsvc 34→53). No more faults. The window is up, the CVDisplayLink drives 60 Hz vsync signals.

But the boot splash stays ("创建 Metal 图形设备", indefinite bar), the game never presents a frame (no
"first presented frame"), and after ~10 minutes the loop is still cycling the same queue. `sample` shows the
guest main thread blocked in `block()` (svc 0x1C condvar, threads.c:463) inside game code reached through
`observed_main`, and workers blocked on their own condvars. Zero file reads after the first 190, zero new
VAMM allocations, ~0.4% CPU.

Diagnostics in place: cv-wait/cv-signal trace (first 40 of each) in `runtime/threads.c`, thread dump via
TOTK_INPUT_FILE "threads" (note: the input-file thread polls with clearerr+retry, but appended lines were
not picked up in practice — use the crash-handler dump or extend the trace limits instead).

Next session leads: (1) raise the cv-trace limits and log the task-queue node contents (the queue nodes are
0x2e0 apart in the guest heap) to see what the loop is processing and what it's ultimately waiting for;
(2) check the shader-service state (totk_xlate in <work>/build/shader-service/) — the game read .bnsh
shaders at 0.000s and PLA compiles shaders through the host translator; a stalled shader job would freeze
the load loop exactly like this; (3) `hletrace 1` via the input file to count which host calls the loop makes.

## The load-loop stall is the boot MOVIE pipeline (same session, decoded)

The main thread's 878M-call WaitSynchronization spin is the game waiting on its media pipeline. Module bases
from TOTK_MEM_LOG attribute the sample's spin frames to **subsdk0** — which is the Android media stack:
`android::ACodec::setMinBufferSize`, `mkvparser::Cluster::Load/Cluster::GetNext` (Matroska/WebM container
parser). subsdk0 is PLA's movie library (same role as TotK's: the SDK movie library built over Android
media). It is parsing a stream — but main imports **no** movie::Decoder symbols, so the decoder is
constructed entirely inside subsdk0 and its vtable is subsdk0-internal; the TotK-style movie_host vtable
override (which hooks main's imports) cannot reach it.

The main thread's spin (svc 0x18 WaitSynchronization on a heap sync object, 878M calls, ~680 ns/call) is the
game waiting for the movie decode to advance; the ACodec decoder needs Tegra NVDEC (nvdrv), which the host
does not have — the same reason TotK needed movie_host.c, but PLA's decoder hook point is inside subsdk0.

Next session: (1) find subsdk0's movie::Decoder construction site (ACodec::setMinBufferSize caller chain,
or the mkvparser's IO callback to identify the data source); (2) hook the subsdk0-internal vtable after
construction (host writes the vtable slots when the object is built — the loader can watch for the
construction via the globals the media threads share); (3) decide the backend: PLA's boot flow plays
its opening/logo movie through this pipeline — an "empty stream" backend (immediate EOS on all events)
would skip the movie and let the game advance to the title screen.

## FS trace with the media stall (final state this session)

With TOTK_FS_LOG=1 the game reads 1,170 files (system shaders .bnsh/.trsha at 0.000s, BFOTF fonts at
0.136s, message grammar .dat at 0.124s, light-probe .bntx/.trlgt at 0.329s) and then the reads **stop at
0.330s** — the remaining loop time is the movie pipeline spin. Zero faults across the whole run with the
glyph gate serving misses. The movie data source is not a plain rom:/ file read (no movie path in the log):
it is either inside a .gfpak the game already read, or delivered in memory.

Session state: the game boots, initializes all subsystems (graphics/audio/input/account/fonts), enters its
main loop with 22 threads alive, renders its first-frame text (glyph misses served), and stalls waiting on
the boot movie decode (ACodec → NVDEC, absent on host). Remaining work: hook subsdk0's movie decoder
construction (same role as TotK's movie_host.c but subsdk0-internal) with an empty-stream backend, then the
game should reach its title screen.

## Handle-0 Waitsync experiment (failed, reverted)

The main guest thread's Waitsync poll uses handle 0 (movie decoder events never created without NVDEC).
Making sig_obj treat handle 0 as "signaled" unblocked the poll but the game immediately hit an SDK Break:
the code after the wait expects real decoded frame data, not just an event signal. Reverted. The movie
pipeline needs a proper backend (nvdrv stub + ACodec decode, or a high-level movie skip).

## Boot movie pipeline — the remaining blocker for the game screen

The game boots, loads 1170 files, initializes 22 threads, renders text (glyph gate), opens audio/input/
account services, and enters its main loop — then stalls waiting for the boot movie (mkvparser/ACodec in
subsdk0). The movie decoder needs NVDEC hardware (absent on host) and its event handles are 0. The game's
text render works because of the glyph gate (SVC_HOST_GLYPH_LOOKUP entry-form cut). The movie needs a
similar approach but the decoder is constructed inside subsdk0 (main imports no movie symbols), so the
vtable hook must target subsdk0's internal construction site rather than main's import table.

## Staged GetLoadState (same session, final)

GetLoadState now returns NotLoaded until `runtime_fs_read_count() > 100`, then Loaded. This lets the game
complete its initial 190-file loading phase (shaders, fonts, grammar, light probes) before proceeding.
Result: 1,170 files total loaded, zero faults, glyph misses served. The game enters its main loop and runs.
The splash screen ("Creating Metal device") stays because the game never presents to the swapchain — the
boot movie pipeline (subsdk0 mkvparser/ACodec) is the remaining blocker between the current state and the
first game frame.

## Movie-thread cut attempted and reverted (same session, final)

Cut the boot movie thread entry (main+0x294971c → return 0 immediately) to skip the movie. The thread
returned, but the game then stalled at a DIFFERENT point: only 4 VAMM allocations, zero glyph misses,
main thread blocked on a different condvar. The game's state machine expects the movie to go through its
lifecycle (init → play → end → signal) — skipping the thread breaks the state machine differently than
the NVDEC absence does. Reverted the movie-thread cut; the glyph lookup cut (SVC_HOST_GLYPH_LOOKUP) is kept.

The movie pipeline is the last major subsystem before the first game frame. It needs dedicated work:
either a subsdk0-internal vtable hook for the movie decoder (constructed inside subsdk0, not reachable
via main's imports), or a minimal nvdrv IPC service, or a game-side "skip movie" patch at the
state-machine level (not the thread level).

## FIRST PRESENTED FRAME (same session)

Three fixes unlocked the first presented frame (boot log 0.752s):

1. **handle-0 Waitsync returns timeout** (runtime/threads.c): the movie waiter polls Waitsync(1,{0},0)
   forever — Horizon says bad-handle for 0, and the caller retries forever. Returning timeout lets its
   timeout branch run, and the boot proceeds to real rendering (281f70c8's experiment returned "signaled",
   which broke differently; timeout is the semantically right answer for a never-signaled event).
2. **The multisample family** (hle_nvn.c): SetCoverageModulationMode + the whole getter/setter set
   (alpha-to-coverage dither, raster samples, coverage-to-color, sample locations). PLA drives the full
   NVN multisample state API.
3. **SPIRV-Cross's spvLinearTextureAlignmentOverride** (nvn_mtl.m shader_load): newer SPIRV-Cross emits
   `constant uint spvLinearTextureAlignmentOverride [[function_constant(65535)]]`; newFunctionWithName
   then yields a function Metal refuses to build pipelines from ("cannot lower module with unresolved
   function constants" — visible after setting MTL_ERROR_MODE=ignore). shader_load now specializes
   constant 65535 = 4 (SPIRV-Cross's default) via newFunctionWithName:constantValues:.

Metal validation is now soft (MTL_ERROR_MODE=ignore set in main) so descriptor errors log instead of
aborting. The first frame is black (the game is past the boot movie section into its first rendering
state; 25 pipelines built), HUD shows 3014x1688. The remaining visual work (what the game paints in this
state, whether the movie plays through) is the next session's business.

## GAME FRAME VISIBLE — language select screen (next session, first capture)

With the first-frame fixes in place and the game left running, the window now shows the game's real UI:
the **language selection screen** at 30 fps (HUD: 3014x1688 internal, 19 new / 44 total shaders, GPU 94%),
text rendered through the shared fonts (简体中文 / English / 日本語 / 한국어 / 繁體中文 …), the selection
cursor on 简体中文. The boot flow passed the movie/logos state on its own (the handle-0 timeout return let
its state machine conclude), and the game is waiting for language input at this point.

The port renders and drives. Next: input (keyboard → nn::hid NpadState) to pick a language and reach the
title/save screens, then the stage-4 playability loop (graphics fixes C4-C8 as they appear, audio levels,
saves, 30→60 fps).

## INPUT WORKS — language confirmed, opening sequence plays (same session)

Two fixes made input land:

1. **The Npad id gate rejected the game's own id.** `pad_states` required `*id == pad_id()`, but
   `pad_id()` follows the host's operation mode (docked → 0) while PLA reads the **handheld** state for
   controller **0x20** every frame (probe: `handheld=61 calls/s, pad_id=0, want=2`). The gate never opened,
   so every state came back zero. Accept 0 and 0x20 alike (both are ids the game declared as supported) and
   gate only on the style bit: `enabled && allowed && (*id==pad_id() || *id==0x20 || *id==0) && (styles & want_style)`.
2. **`TOTK_INPUT_FILE` is relative to the work folder, not the game folder.** `make boot` runs the native
   binary with cwd = `$(WORK)` (PLA_build), so `TOTK_INPUT_FILE=keys.txt` opens `PLA_build/keys.txt`. Earlier
   key tests appended to `games/pla/keys.txt` and were never read (the game kept replaying stale `key 40`
   lines). Write the script to `PLA_build/keys.txt`.

Result: `key 38` (J = A) at the language screen → the game takes the confirmation, the menu disappears, the
loading transition runs, and the **opening cutscene plays** (golden light burst, 71 shaders built, 30 fps).
There is also a diagnostic `PLA_INPUT_TRACE=1` in `pad_states` printing calls/s, enabled/allowed/styles/
pad_id/want per two seconds when input needs investigating.

## Opening cutscene runs; the Y prompt does not advance it (same session, last state)

After the language confirmation the game plays its opening sequence (the golden light burst renders and
animates, 71 shaders, 30 fps, zero faults). The screen shows a "press Y" prompt in the bottom-right and a
frame of a dark UI panel; pressing A (key 38) and Y (key 32) leaves it unchanged for the next 60+ seconds.
No fault, no unimplemented NVN call — the prompt simply is not consumed.

Next leads: (1) the prompt may be driven by a different button mapping than keycode 32
(`button()` maps 32→bit3); confirm against the game's own on-screen glyph by trying each of J/K/U/I and the
d-pad; (2) the sequence may wait on the movie pipeline after all (the handle-0 timeout only unblocked the
first waiter) — check `Waitsync` traffic while the prompt is up; (3) the UI panel above the Y glyph may be
the "skip cutscene" prompt whose accept path needs the audio thread (no audio device on this host yet —
audren renderer opens but no output device is started).

## Text is blank inside the dialogue box — the glyph gate serves blank glyphs (reported by the user)

Screenshot review caught what the logs hid: the subtitle/dialogue panel under the opening cutscene is
empty. The cause is the glyph gate itself (C9-era fix): `SVC_HOST_GLYPH_LOOKUP` returns a **zero block**
for every miss, so every Chinese glyph the game asks for renders as nothing.

Evidence gathered this pass:
- 3 920 misses per boot, and the character codes are real text: `0x5728 圣`, `0x8bb0 记`, `0x5f55 录`,
  `0x6b63 正`, `0x2026 …` (982), `0x20` space, `0x3f ?` — the game is asking for the characters it wants to
  draw and the lookup returns NULL for all of them.
- The game DOES fetch the fonts: `PLA_FONT_TRACE=1` shows GetSharedFontLoadState / GetSharedFontAddress /
  GetSharedFontSize all hit, with addresses `GetSharedFontAddress(4) -> 0x78a0904d38 (3672144 bytes)`,
  `(2) -> …(124028)`, `(1) -> …(7848200)`, `(3) -> …(3580260)`.
- Changing the pointer from "8 bytes past the BFTTF header" to the file start (`PLA_FONT_HEADER_SKIP=0`) does
  not change the miss count (3 899 vs 3 920), so the misses are not an offset problem.
- The bitmap font in RomFS (`bin/font/bmp/font_24_bold.bffnt`) explains why the **language menu** text renders
  fine while in-game text does not: the menu uses the bitmap font, the game text goes through this lookup.

So the missing piece is the game's character→glyph table (manager+0x1888 / +0x1c30): it holds ~184 entries
and none of the CJK characters the text needs. Two ways forward:
1. Let the game fill it: our entry-form cut replaces the whole lookup, so any "miss → build the glyph from
   the font" path *inside the lookup* never runs. Switching to a **call-site cut** (replace the `bl` at
   `main+0x11f88c` with the host svc, keep the original function intact, fall back only on a real NULL
   return) would preserve the game's own path.
2. Build the table host-side from the BFTTF files (needs the BFTTF glyph directory format decoded).

New diagnostics kept: `PLA_GLYPH_TRACE=1` appends every requested character code to `glyph-miss.txt` (in the
work folder) so the needed character set can be analysed offline; the font-symbol hits print under
`PLA_FONT_TRACE=1`; the font header offset can be switched with `PLA_FONT_HEADER_SKIP` (8 default, 0 = file
start).

## Glyph manager ground truth (PLA_GLYPH_DUMP, same session)

A safe `mach_vm_read_overwrite` dump of manager+0x1800..+0x2000 (written to `glyph-mgr.bin`) settles the
layout the lookup at main+0x121cb4 uses:

| offset | contents |
|---|---|
| `+0x1888` | 32 flag bytes: `02 02 02 03 02 00 … 00` — only slots **0..4** are initialized (values 2/3 = face types) |
| `+0x18a8` | 32 pointers (8 bytes each), slots 0..4 set: `0x…5ce8, 0x…5cea, 0x…5cec, 0x…5cee, 0x…5cf1` — 2–3 bytes apart, all into one small heap stream; slots 5..31 zero |
| `+0x19a8` | `00 01 02 03 …` — the byte run right after the 32-slot pointer array (this is what an out-of-range index reads) |
| `+0x1c30` | a pointer to the per-character map (two adjacent pointers seen: `0x1100674939024`, `0x1100662287392` — real 36-bit values) |

The game asks the lookup for **idx = 32** (`char=0x3f idx=32`, then every later query the same) while only
slots 0..4 are initialized. `flag[32]` therefore reads the low byte of the first *uninitialized* pointer slot
(= 0x…5ce8's LSB 0xe8, nonzero), the check passes, and the pointer fetch at `+0x18a8 + 32*8 = +0x19a8`
returns the byte run `0x0706050403020100` as if it were a pointer — that is exactly the `fault=0x07060504…`
seen when the dump dereferenced it. So the original lookup crashes on a **size/face index the manager never
initialized**, not merely on a missing character.

That reframes the text problem: the host must make the game initialize the face the text system wants (idx
32) — or answer that index itself. Next session: find who fills `+0x1888/+0x18a8` (the initializer ran 5
times; what event should make it run for index 32?), by watching those two slots with the svc-granularity
shadow technique already in `runtime_dispatch`; the byte run at `+0x19a8` suggests the face table is built
per size on demand and something skipped the sizes the game actually uses.

Diagnostics: `PLA_GLYPH_DUMP=1` writes `glyph-mgr.bin` (3 × 0x800-byte snapshots of manager+0x1800) plus one
log line per capture; `PLA_GLYPH_TRACE=1` still appends every requested char code to `glyph-miss.txt`.

### Correction to the row above (same session, static analysis)

The "32-slot flag/pointer tables" reading was wrong. `main+0x1074890` contains a ladder of
`adrp/add/str x13, [x8, #0xNNNN]` storing **string pointers** at exactly the offsets we probed:

```
0x1848, 0x1868, 0x1888, 0x18a8, 0x18c8, 0x18e8, 0x1908, 0x1928, 0x1948, 0x1968, 0x1988, 0x19a8, 0x19c8
```
stride **0x20**, each entry 0x20 bytes (name string + fields). So `+0x1888` is one entry of a 0x20-stride
record table, not a flag array, and `+0x18a8 + 32*8 = +0x19a8` is simply the **next record's name pointer** —
which is why "flag[32]" read 0xe8 (the low byte of that pointer) and why the "stream pointer" was garbage.

What still stands from the dump: the game calls the lookup with `w2 = 32` on every query, and the object at
`r[0]` holds a record table around +0x1848..+0x1dc8. What `0x123270` does with `w2` and which table it
walks therefore needs one more disassembly pass over the *whole* function (not just the prologue), plus a
dump of the object's vtable/head to identify the class.

Two candidates for the next pass, both cheap:
1. dump `[r[0]+0..0x60]` and follow the vtable to symbols [objdump over the first 8 words];
2. disassemble `0x121cb4` to its `ret` (we have only the prologue so far) and give the map pointer a
   proper read to see whether it indexes by character or by record.

## Glyph lookups now forward to the game's own code (17 call-site cuts) — text still blank

Replaced the single entry-form cut (which replaced the whole lookup, and with it the game's virtual
fallback resolver at `[this+0x1bd8]`) with **17 call-site cuts**, one per `bl 0x123270` in
`main+0x11f88c..+0x11ff9c`, each with its own host svc (`SVC_HOST_GLYPH_LOOKUP_0..16` = 0x82..0x92 in
`hle.h`, call-site form, original `bl 0x123270` verified). The handler (`runtime/services.c`) now:

```c
lookup_fn fn = (lookup_fn)runtime_guest_function(0x123270);   // the untouched original, by guest offset
void *r0 = fn ? fn(r[0], r[1], r[2], r[3]) : NULL;
regs[0] = r0 ? (uintptr_t)r0 : (uintptr_t)blank;              // substitute only on a real NULL
```

Verified: structck OK (已登记剪口=17, 失败项=0), equiv OK (600/600, sites 34 785, 0 divergent), second
rewrite byte-identical, `runtime_guest_function` added to loader.c.

**The original lookup returns real glyph records, not NULL** — `glyph lookup forwarded char=0x3f idx=0x20
-> 0x10045cc6000`, `char=0x20 idx=0x2a -> 0x10045cc60b0`, etc. So my earlier "lookup returns NULL" reading
was an artefact of the entry-form cut itself; the null deref that started this thread was caused by a
*stale* path, not by the lookup failing.

**Text is still blank** in the dialogue panel, so the missing piece is downstream of the lookup: the glyph
records point at a font texture atlas that has no pixels (the game never rasterised the CJK glyphs into it,
most likely because the pl shared-font import that should feed it never ran — see E4/the
`GetSharedFontAddress` hits). Next: watch the atlas texture (which texture id the glyph records reference,
and whether any upload writes it) with `DM_NVN_REGLOG` and the RT/texture diagnostics; the record layout is
184 bytes per glyph (`umaddl x0, w22, #0xb8, this->base`) and the record at `this+0x10` is what the draw
path reads.

Diagnostics: `PLA_GLYPH_TRACE=1` (char codes to `glyph-miss.txt`), `PLA_GLYPH_DUMP=1` (manager snapshots),
the forward log line above prints char/idx/returned pointer for the first 10 lookups each run.

## TEXT RENDERS — player-name screen reached, Chinese UI text correct (same session)

After the 17 call-site cuts (glyph lookups forwarded to the game's own code), the game plays through the
opening cutscene into the **player-name entry screen**, and the Chinese UI text renders correctly:

- `你的名字是？` (title)
- `最多 6 个字 · 用键盘输入，回车确定，Esc 取消` (hint)

So the blank dialogue panel that started this whole thread was an empty subtitle slot, not broken text —
the glyph path (lookup → record → font atlas) works as soon as the game owns it end to end. The earlier
3 920 misses per boot came from the host replacing the lookup; with the forward architecture the game's own
fallback resolver populates the records.

State: 30 fps, 71 shaders, zero faults, keyboard input driving menus, naming screen waiting for text entry
(the host has a swkbd override plus the diagnostic `kbtype <text>` / `kbenter` commands in
`TOTK_INPUT_FILE`).

## Character confirmation screen with a rendered 3D portrait (same session)

Typing the name through the host keyboard completed the flow: the swkbd request (`你的名字是？`, type 5,
1–6 characters, UTF-16 out) was answered with "Pal", and the game advanced to the **character
confirmation screen**:

- the Arceus logo renders top-left;
- a full 3D portrait of the protagonist renders on the left (hair, eyes, skin — correct shading, no black
  silhouette, so the C6–C8-class lighting paths are behaving);
- two buttons on the right, cursor on the first (their labels are still blank — likely the same glyph/atlas
  story for button text, to chase in stage 4).

State: 30 fps, 71 shaders, zero faults, keyboard + swkbd input working, saves not exercised yet, audio
renderer open but no output device started.

The port now runs the game's own opening flow end to end: language → cutscene → name entry → character
confirm, all rendered by the host's NVN→Metal layer.

## Into the world loader — transform-feedback family implemented, loading is heavy but live (same session)

Character confirmation → the game enters its **field loader**: a Poké Ball progress indicator spins in the
bottom-right, shader count goes 71 → 172 (101 compiled during the transition), then settles at 30 fps.

One new NVN entry point appeared and is now implemented as accepted-and-recorded no-ops (Metal has no
transform-feedback stage): `BindTransformFeedbackBuffer(s)`, `Begin/End/Pause/ResumeTransformFeedback`,
`DrawTransformFeedback` — all seven from names.txt. After that: **zero unimplemented NVN calls, zero faults**
through the transition.

The loader does real work rather than deadlocking: a thread dump at that point shows the main thread at
1.26M svc and two loader threads at ~921k each, all cycling through `svcArbitrateLock` /
`WaitSynchronization` on their own futexes, while GPU utilization reads 0 %. The progress indicator has not
advanced past the initial frame in ~8 minutes of wall clock, so the loader is either extremely slow on this
host or parked on a condition its own threads expect from a source that is not there (audio device, save
data, or an asset stream).

Next leads, in order: (1) `TOTK_FS_LOG=1` during the loader to see whether the field archives
(`ha_area*.gfpak`, 70–97 MB each) are being read at all; (2) watch the two 921k-svc threads' guest return
chains to name their loops; (3) check whether the audio renderer's device-open path is what the loader waits
on (audren sessions open but no output device is started on this host).

## Two rendering fixes for the field loader's passes (same session)

1. **Color-attachment write mask from the MSL** (`nvn_mtl.m`): `NvShader` gained `color_mask`, computed at
   shader load by scanning the fragment MSL for `[[color(N)]]`; `pso_desc` now clears the write mask (and
   skips blending) for attachments the shader does not declare. NVN tolerates this; Metal rejects the whole
   pipeline. (The reflection json carries no fragment-output list — `raster` is empty — so the MSL scan is
   the only source of truth here.)
2. **Retry on the validation error**: if `newRenderPipelineStateWithDescriptor` still fails with
   `...that is required for blending`, the pipeline is rebuilt once with attachments 1..7 blended off and
   masked out. **Not yet observed firing**: the 12 per-run failures named `cfmt0 115, samples 1` persist in
   the last test, and the retry's "按不写处理" line never appeared, so the error text reaching C string
   comparison is not what the log prints (the log prints `e.localizedDescription`, and the comparison uses
   `containsString:` on the same object — needs one more look with a debug print of the raw description at
   the failure site).

Also confirmed during this pass: the field loader reads **2 418 files up to t≈287 s** and then stops reading
entirely (last reads: `ef_*.ptcl` field particles, `bin/haxe/release/event/ev_main_000040.blua`, then
`bin/message/Simp_Chinese/script/chap_00.dat/.tbl` — the opening chapter's script and dialogue). Save storage
is mounted (`user-data/save/0dfdb13c…`). Zero faults. The Poké Ball progress indicator keeps spinning after
that point, so the stall is after the chapter-0 message data is loaded.

## Loader stall is not audio — the movie thread is the 99 % CPU spinner (same session)

`PLA_AUDIO=0` (silent real-time sink) changes nothing: the loader still reads exactly 2 418 files and stops at
t≈286.7 s. So the audio device is not what the loader waits on.

Cross-referencing the two thread dumps: the thread that sits at **99 % CPU while the loader spins** is
`h=0x1017` (`wait=0`, `last_svc=0xb` = SleepThread, nsvc ≈ 42 000) — the same thread that earlier sampled
inside **subsdk0's `mkvparser::Cluster::Load`** (the Android media stack). The other loader threads are parked
on `WaitSynchronization` futexes as usual. So the field loader is most likely waiting for a **movie to play or
finish** (PLA plays a short in-engine/movie transition after character creation) and the movie pipeline cannot
deliver frames (no NVDEC), leaving the parser spinning.

Everything else about this state is clean: zero faults, zero unimplemented NVN calls, save storage mounted,
172–194 shaders built, 30 fps presentation.

Two ways to settle it next session, both narrow:
1. Confirm the movie: watch the three `subsdk0` movie symbols (`movie::NnCodecPlayer::GetCurrentPlaybackPosition`,
   `movie::DecoderImpl::GetInputBuffer`, `NnCodecVideoOutputReceiver::Initialize`) by name-override logging —
   if the player object exists and its position never advances, that is the stall.
2. Then apply the D3 treatment to this call site: either make the player report end-of-stream immediately
   (so the state machine concludes the movie) or hook the subsdk0-internal decoder vtable after construction.

The 12-per-run `Fragment shader does not write to render target color(0), index(1) that is required for
blending` failures are still present and their retry path does not fire; the log prints the same
`localizedDescription` that the retry compares against, so the comparison needs a print of the raw string at
the failure site to see why `containsString:` misses.

## Re-read the (greatly expanded) porting skill — the movie thread is a red herring (same session)

The skill has grown to 113 problem entries across 8 new reference docs. Two entries bear directly on this
stall:

- **D3** ("boot stalls at the logo, `nm main | grep movie` empty, subsdk0 has mkvparser/ACodec/NuPlayer"):
  the documented advice is **do nothing** — the F3 handle-0 timeout is what lets boot skip the animation, and
  cutscene playback is explicitly deferred. So the subsdk0 mkvparser thread spinning at 99 % CPU in the field
  loader is very likely **not** what the loader waits on.
- **D7** (movie::Decoder MediaCodec protocol) is the full backend work needed *when* cutscenes must play
  (16-slot output rings, EOS stickiness, pictureless+flag drain, `nv12-uv-stride`, 250 ms flush withholding,
  native-texture mode). That is a dedicated task, not a step in this session.

So the loader stall is reclassified: movie thread spins (expected), audio ruled out (`PLA_AUDIO=0` identical),
save data mounted, 2 418 files read to t≈287 s, zero faults, then the Poké Ball spinner never advances while
three threads cycle ~1 M svc each on their own futexes.

Next session's plan (tight, per the skill's triage tree §1–§5 and the "STOP" gates):
1. `mscan`/lldb the two 921 k-svc threads' guest return chains to name what they wait on — the triage tree
   says not to add sleeps before the wait semantics are understood.
2. Check whether the loader's progress needs one of the **12 failing pipelines** (`required for blending`) —
   fix that first by printing the raw `localizedDescription` at the failure site; a nil pipeline can leave a
   loading render pass without its draw.
3. If the loader turns out to wait on the chapter-0 event script, dump the `.blua` VM's state (haxe/Lua) the
   same way the glyph manager was dumped.

## Loader threads named: condvar round-trips inside unexported game code (same session)

The two ~933 k-svc loader threads (`h=0x1002/0x1003`) both sit in the same guest frame:

```
sdk+0xe6d344 <- sdk+0x345bc4 <- main+0x296d9a4 <- main+0x29497b4 <- sdk+0x349d98 <- dm_thread_enter
```
`sdk+0x345bc4` = `nn::os::WaitConditionVariable+0x44`, so they are in a **condition-variable wait/signal
round-trip driven by game code at `main+0x29497b4`** (called from `main+0x296d9a4`). Neither address has a
dynsym name (nearest exported symbol is thousands of bytes away), so naming them needs the static-investigation
route the skill describes for stage 0.5: nxnative's `nso_fnmap`/`nso2elf` plus a Ghidra project, or our own
`func_starts` + a function-signature dump.

The main thread's chain resolves into the data segment (`main+0x42a8890`), i.e. the frame-pointer walk is
unreliable there; its 1.27 M svc are `svcArbitrateLock` traffic.

Reading: every loader thread is *busy*, not blocked — two workers ping-pong on condvars and the main thread
arbitrates locks. The loading indicator simply never advances, so this is either a genuine long load (see the
2 418-file read burst ending at t≈287 s) or a loop whose termination condition never becomes true somewhere
in `main+0x29497b4`'s callers. Distinguishing those two needs the function named, which is the next step.

Concrete next-session plan:
1. Name `main+0x29497b4` / `main+0x296d9a4` (Ghidra or nxnative `nso_fnmap`); they sit in the same region as
   the movie-thread entry `main+0x294971c`, so they are probably the media/loader worker pool.
2. With the name known, set a breakpoint on the loop's exit condition and see which flag never flips.
3. In parallel, fix the 12 `required for blending` pipeline failures (print the raw error string at the
   failure site first) — a nil pipeline can leave the loading screen's render pass incomplete.

## The 12 pipeline failures: the shader declares color(1) but never writes it (same session)

Chased to a conclusion with an ASCII marker on the retry path:

```
[dm] [nvn-mtl] [pla] pipeline retry without extra color attachments -> still nil
```

So the retry runs (the earlier "no marker" was a false negative — `strings` on macOS only prints ASCII, so
the Chinese marker was invisible), and clearing blending + write masks on attachments 1..7 does **not** fix
it. Together with the MSL inspection (`struct main0_out { float4 m_32 [[color(0)]]; float4 m_34
[[color(1)]]; }`), the real situation is: **the fragment MSL declares a color(1) output but the code path
never assigns it**, so Metal's compiled function has no color(1) output while the pipeline asks for blending
there.

The fix has to be at the shader-source level: after translation, zero-fill any declared-but-unassigned
colour output (`out.m_34 = float4(0);` before `return out;`). That belongs in the MSL rewrite the graphics
layer already performs for depth/border variants (`fs_src_without_depth`, `border_src`), not in the
pipeline descriptor.

Neither this nor the movie thread is what stops the field loader; the loader threads' guest chains resolve to
the generic thread trampoline (`main+0x294971c`), so naming the real worker body needs a Ghidra project
(skill stage 0.5, `nso2elf` + Ghidra) — our dynsym has no entry for `main+0x296d6b8`.

## FIXED: the 12 pipeline failures — zero-fill unassigned declared colour outputs at MSL level

Root cause confirmed and fixed. `msl_zero_unwritten_colors()` (nvn_mtl.m) parses the fragment MSL's
`struct main0_out`, finds every field declared with `[[color(N)]]` that the function body never assigns
(no `out.<field>` anywhere), and inserts an explicit `out.<field> = float4(0.0);` immediately before
`return out;` — on the return path the compiler cannot elide the output, so Metal sees the fragment really
writes that attachment. Applied to fragment sources in `shader_load` before compilation.

Verified in one full run to the field loader: **`建渲染管线失败` count 12 → 0**, zero faults, and the shader
count rose 194 → **384**, i.e. the pipelines that used to be dropped now build (the game went on to compile
far more of the field's shader set).

Earlier descriptor-side attempts (write-mask from `[[color(N)]]` scanning, and a blind retry clearing
attachments 1..7) are kept as harmless fallbacks; the ASCII marker proved the retry ran and still returned
nil, which is what pointed at the shader source rather than the pipeline descriptor. Note for the toolbox:
`strings` on macOS only prints ASCII, so a Chinese MLOG marker looks absent even when the code is linked —
verify with `nm`/a run, not `strings`.

## Loader worker body structure (same session)

`main+0x296d6b8` (the body the loader threads actually run; the frame in the dump was the generic thread
trampoline at `main+0x294971c`) starts as a queue/teardown walk:

```
ldr  x8, [x0, #0x5e8]      ; job/resource list head
cbz  x8, fail
add  x8, x19, #0x304;  ldar w8, [x8];  cmp w8, #1
   → if state==1: blr [x19+0x310] (vtable slot), then blr [x19+0x318] if set, then stlr #2 to +0x304
ldr  x8, [x19, #0x5e8]; add x20, x19, #0x5c8
ldr  x22, [x8, #0x58]      ; walk the node chain at +0x5c8, reading [node+0x58]
ldr  w24, [x22, #0xc]      ; per-node field
```

So it is a **one-shot state machine (state 1 → invoke → state 2) over a linked job list**, not a spin loop —
each loader thread runs this, waits on its condition variable, and runs again. That is consistent with the
two workers ping-ponging on condvars: the pool is alive and being walked; whatever never completes is inside
the jobs themselves.

Next session: name the job vtable slots (`+0x310`, `+0x318`) and the job object type — the callbacks are what
decide completion. A Ghidra project from `nso2elf` output is the practical way (skill stage 0.5); the dynsym
has nothing near these addresses.

## Pipeline failures: three hook points tried, the field IS assigned — story is still open

Measured with python (not `grep -c`; G14 bit me twice on this file — the earlier "12 → 0" was a false zero):

- All 12 failures are the **front** path (`【红】建渲染管线失败` from `pso_get`), none are the background warm.
- `msl_zero_unwritten_colors()` now wraps **all three** fragment compile sites (`shader_load`, the
  depth-stripped variant in `fs_fn_without_depth`, and the border variant in `fs_fn_border`), and it logs
  when it actually inserts a zero write — **it never fires**, i.e. for these shaders every declared
  `[[color(N)]]` field *does* appear in the source as `out.<field>`.
- So the "declared but unassigned" story is wrong for them: the assignment exists yet the compiled function
  still has no color(1) output. That points at the assignment being unreachable or the output being dropped
  for another reason (e.g. written only under `discard_fragment()`, or a struct/function mismatch).

State of the build after all of this: **0 faults, 1 first presented, 0 host aborts, 384 shaders**, the field
loader still stalls after its read burst (2 415 files, last: `battle.blua`, `vibration.gfpak`,
`preset_camera/transform.bin`).

Next for these 12: dump the failing shader's MSL and look at where `out.m_34` is assigned (the border variant
key `…-fcc32b6e26b0_b` names the exact file). If it is inside a branch, the cheap fix is an unconditional
zero write at the end for that specific output — but only after seeing the assignment.

## The 12 "required for blending" pipelines: NVN's second colour source, not an unwritten output (same session)

The failing pipelines (`ps f4f36a03…` / `ff45da02…`, `cfmt0 115` = RGBA16Float) blend target 0 with an NVN
**SRC1** factor, so Maxwell reads the fragment's colour output 1 as target 0's second source. Metal has that
feature but only through `[[color(0), index(1)]]`; the translated MSL declares a plain `[[color(1)]]`, so Metal
rejects the pipeline with exactly the message in the log. The output **is** assigned (`out.m_34.w = _117`), which
is why the declared-but-unassigned zero-fill wrapper never fired and why descriptor-side retries could not help.

- `msl_dual_source()` rewrites the colour-1 declaration to `[[color(0), index(1)]]`; `fs_fn_dual()` compiles that
  variant (also depth-stripped), and `fs_fn_border(..., dual)` compiles it into the border-colour variant when the
  draw needs both.
- `pso_dual_needed()` selects it when target 0's blend names a SRC1 factor. Attachment 1 is then masked: Maxwell
  does not write the second source to a colour attachment. A shader without a colour 1 output maps the SRC1
  factors to ZERO/ONE (the hardware reads a zero second source) instead of failing the pipeline.
- Verified with a standalone Metal program on this Mac: the original MSL with `Source1Alpha` fails with the same
  validation error; the rewritten MSL builds, and plain SourceAlpha / One are unchanged. In a full run the
  `建渲染管线失败` count is **0** (was 12), and the same log position (line 626) now prints
  `dual-source pipeline: fs f4f36a03… target0 src=2/2 dst=18/18 eq=1/1` — source ONE, destination
  `SRC1_ALPHA` (0x12), exactly the dual-source case.

## The field-loader Poké Ball: Apple silicon's 64-byte exclusive granule vs. a clang stack spill (same session)

After the pipelines were fixed the loader still stopped at its Poké Ball. The two loader threads waited on their
condition variable; the main thread sat in `main+0x1e916b8` with its SVC counter frozen — a user-mode livelock, not
a wait. The code is libc++'s `atomic_exchange`:

```
ldaxr x9, [x8]        ; x8 = sp+0xc8
str   x9, [sp, #0xa0] ; spill of the old value, same 64-byte line as x8
stlxr w9, xzr, [x8]
cbnz  w9, -4
```

On Apple silicon the exclusive monitor covers **64 bytes**, and any store to that line clears it, including one by
the same thread; the STXR never succeeds. A standalone test measured 100000/100000 STXR failures for a scratch
store in the same 64-byte line and 0/100000 for the next line; the same address fails 100000/100000. lldb on the
live process showed the slot changing between retries while `w9` stayed 1. On the Switch's Cortex-A57 the pattern
works (the granule is smaller / same-thread stores elsewhere in the line do not clear it).

Fix: a compatibility patch, always applied at load (`runtime/game_patches.c` `apply_compat`, checked word for word
against the rewritten image), moves the spill after the successful exchange and uses `w16` (IP0, dead after the call
just before) as the STXR status:

```
ldaxr x9, [x8]; stlxr w16, xzr, [x8]; cbnz w16, -4; str x9, [sp, #0xa0]
```

A capstone scan of all four modules for exclusive load/store pairs with an intervening store of the loaded value
found only this stack-frame-internal site (`main+0x1e916b8`); the object-relative candidates in `sdk` are false
positives on closer reading (their stores are on other paths or before the load).

Verified: the loader completes and the run reaches the field — the Obsidian Fieldlands beach renders (rift in the
sky, dock, hut), 16,201 frames presented, 30 fps, 0 pipeline failures, 0 faults, exit 0. Screenshots:
`/tmp/pla_now1.png`, `/tmp/pla_beach_b.png`.

## Next

- The beach scene's dialogue panels are empty (the beige box, no text); the title, language and name screens do
  render text, so this is a different text path, not the glyph gate. Advance the intro with A and watch whether
  text appears once the professor speaks; if not, dump the dialogue render pass and its font/size.
- Then: player character and free movement, the first battle, save round-trip, audio (Wwise/hwopus already open).

## User report: only the language page's text is right; the font atlas holds just the ellipsis (same session)

Reproduced. The dynamic text draws exist and the game asks for real CJK — `PLA_GLYPH_TRACE=1` records 2,553 CJK
lookups and 982 ellipses during the intro (the language warning's characters among them), so the lookup is not
substituting '?'. The text is drawn by fragment shader `0f1f2c66…`, one quad per run, sampling a **2048x2048 R8
glyph atlas** (serial 3978; not a render target in any pass, so it is filled from guest memory).

`DM_NVN_TEXPNG_FS=0f1f2c66` at the character-confirm screen dumped that atlas: its only non-black pixels are the
**ellipsis glyph in two sizes** (bbox 1,1916..113,2039). 409 of 524,288 8-byte words are nonzero. Every CJK glyph
the confirm labels and the dialogue ask for is absent from the atlas.

The language page is not proof that the dynamic path works: its UI pass samples small pre-sized text textures
(20x108 / 254x108 / 240x64, e.g. serials 4114/4115/4120) rather than the atlas, and the name-entry prompt
("你的名字是？") is the **host's swkbd bar** — a native Cocoa control — not game-rendered text. The earlier
"TEXT RENDERS" entry is corrected: it was the host UI.

So: the atlas upload/path works (the ellipsis reached it), but the CJK glyph build never rasterises a glyph into it.
The fonts in play: the game's own `/bin/font/FOT-RodinNTLGPro-DB.BFOTF`, `FOT-UDKakugoC80Pro-DB.BFOTF`,
`or_font.BFOTF`, `font_data.prmb`, a bitmap font `font_24_bold.bffnt` (opened at ~82 s), and the system shared fonts
(pl:u types 1–4). Next: make the game's CJK glyph build run (it is the miss path behind the forwarded lookup), or
decode BFOTF/BFTTF host-side and serve the bitmaps into this atlas/record form. The ellipsis shows the format and
upload are not the blocker.

The beach's missing characters are a separate open question: the confirm portrait is a 2D face texture, so it does
not prove the 3D character path; a beach passdump is needed to tell "never spawned" (the intro event waiting on the
broken text) from "drawn but culled" (e.g. occlusion queries).

## Refinement: ALL dynamic text is blank, not just CJK; language names are baked UI (same session)

- The language screen's warning "一旦决定将无法在中途更改。" is also blank in the screenshot (`/tmp/lang_pd.png`):
  only the nine language names render, and those are pre-baked UI textures. The name banner on the confirm screen
  ("Pal", Latin) is blank too. So the dynamic text path fails for **Latin and CJK alike**, and the ellipsis-only
  glyph atlas is the font engine's fallback glyph, not a CJK-only gap.
- The 2048x2048 R8 atlas (serial 3978) is filled from guest memory; the ellipsis is its only glyph. The text draws
  (fs 0f1f2c66, one quad per run) sample it; nothing else is in it. The font manager therefore has no usable glyph
  for any requested character: its font registration/char-map build is the missing piece, upstream of the (working)
  draw and upload paths.
- `PLA_FONT_HEADER_SKIP=16` (serve the BFTTF header instead of the body) changes nothing on the confirm screen.
- New diagnostic: `PLA_GLYPH_REC_CJK=1` dumps the 0xb8-byte glyph record plus the 0x40 bytes at its +0x10/+0x58
  pointers for the first CJK/ellipsis lookups (services.c), to compare a char that rasterises with one that does not.

## Beach characters: not NVN occlusion-query culling (same session)

- With `flag occl_visible 1` (the host forces every occlusion query to report visible) and a 30-frame `querylog`,
  the beach still shows no characters and no `[query]` result lines at all: PLA does not use NVN occlusion queries
  on this path, so the missing characters are not that culling.
- The beach frame (`passdump`) has a 64x64 target with 331 object draws (occluder/depth/culling work) and a few
  colour passes into the 1920x1080 scene target (3 + 3 + 2 + 40 + 2 draws; some 3996/10095-vertex meshes).
  Whether the player/NPCs are among those draws, or the game never spawned them because the intro dialogue event
  is blocked by the blank text, is the next question; the camera and the fire effect do advance with A.

## Font data is fine: the BFOTF files arrive complete and are decoded to valid OTF in memory (same session)

`PLA_FONT_IO=1` (runtime/romfs.c) logs every read of a font file with its guest destination. The game reads
`FOT-RodinNTLGPro-DB.BFOTF` (3,440,016 B), `FOT-UDKakugoC80Pro-DB.BFOTF` (3,540,728 B), `or_font.BFOTF` (9,720 B)
and `font_data.prmb` (21,896 B) **completely, in one read each**, into fixed guest buffers (the same addresses
every run). lldb on the live process: the buffers now hold the **decoded OTF** — `7f 9a 02 18` followed by
`4f 54 54 4f` ("OTTO"), version 0x000f0080, `BASE`, `CFF `, `GPOS` — so the SDK's scalable-font decode works,
the file access is complete, and the fonts are present in memory in the modern OTF form.

The failure is therefore inside the SDK's `nn::font::ll::ScalableFontEngine` / font-manager registration path: the
faces are loaded but their char→glyph maps/glyphs do not reach the atlas (only the fallback ellipsis does). The
glyph manager's table was ~184 entries in the earlier ground-truth dump; the big OTF faces are apparently not
registered into it. Next probes: the font engine's load/SetFont calls and the manager's map before/after them
(the manager tables at +0x1888/+0x1c30 from the earlier session), and whether the SDK font code's rewritten
sites behave (it is part of `sdk`, 12,085 rewritten sites, equiv 0 divergent but the font path is a large client).

## Reference: the nxnative build of this game renders the text; shared fonts are the difference (same session)

The user pointed at the working native build (`/Volumes/bit2t/switch/games/宝可梦 阿尔宙斯/Pokémon Legends - Arceus.app`,
the nxnative host). Its language page shows the dynamic title "要使用哪种语言进行游戏？" and the warning
"一旦决定语言，将无法在中途更改。"; its log says:

```
[dm] shared font (standard.ttf): /System/Library/Fonts/Supplemental/Arial Unicode.ttf (23278008 bytes, BFTTF 'decrypted' header + TTF as is)
[dm] shared font: BFTTF header + TTF (SDK 11, main imports ScalableFontEngineHelper::Decode)
```

So the reference serves a **plain TTF** (macOS Arial Unicode) behind the decoded BFTTF header for every shared-font
type, and **forces `ScalableFontEngine::LoadFont`** so a caller that passes the header address gets the TTF payload.

What this port learned from it, in order:
- Our shared fonts were the **archive (obfuscated) BFTTF**: big-endian words XORed with `word0 ^ 0x7f9a0218`,
  header = marker/length. The pl service decodes them on hardware. Serving the file past the header made the
  game's `Decode` (which PLA calls for its own BFOTF files) see ciphertext. The header form + in-place decode is
  now in `runtime/shared_font.c`; the TTF stand-in (Arial Unicode, marker + TTF) is the default and matches the
  reference. `PLA_FONT_HEADER_SKIP=8` still selects the old TTF-past-header form.
- The game's CJK glyph lookups do not reach the SDK engine: `PLA_GLYPH_TRACE` shows 100/100 CJK lookups returning
  **NULL** (the '?'/space ones return records), and wrapping `CheckGlyphExist`/`AcquireGlyphmap` shows only
  '?'/space. `Decode` is called only on the game's own BFOTF buffers, never on the shared font. The missing piece
  is the same as the reference's forced `LoadFont` takeover: a caller hands the shared font's header address
  straight to `LoadFont`, which fails (error 403), so the CJK face is never registered.
- `runtime/font_hook.c` implements that takeover (patch the sdk LoadFont thunk's PLT entry with a host stub that
  strips the shared font's 8-byte header and calls the real implementation). It is env-gated
  (`PLA_FONT_LOAD_HOOK=1`); the first attempt installed but the game then never reached its font loads, so the
  patch path/ABI still needs work. Default runs are unaffected.

Diagnostics added: `PLA_FONT_IO` (font file reads with destinations), `PLA_FONT_ENGINE` (LoadFont/SetFont/
CheckGlyphExist/AcquireGlyphmap/Decode wrappers with return codes), `PLA_FONT_LOAD_HOOK`.

## FIXED: dynamic text — the shared font header was reaching FSS_load_font; strip it there (same session)

The text works now: the language page shows "要使用哪种语言进行游戏？" and "一旦决定语言，将无法在中途更改。", and the
beach dialogue renders "？？？ / 让人不知道该怎么形容…… / 你在这附近有认识的人吗？" (screenshots `/tmp/ho_1.png`,
`/tmp/ft_beach1.png`). The chain, all verified:

- The shared fonts must be **plain TTFs behind the decoded BFTTF header** (the reference nxnative build serves
  Arial Unicode exactly this way). `runtime/shared_font.c` now decodes the archive BFTTF in place (big-endian
  words XOR `word0 ^ 0x7f9a0218`) and defaults to the Arial Unicode stand-in; the address is the header and the
  size includes it (PLA imports `ScalableFontEngineHelper::Decode`).
- `PLA_GLYPH_TRACE` showed 100/100 CJK lookups returning NULL while '?'/space returned records, and `Decode` is
  called only on the game's own BFOTF buffers, never on the shared font: the shared font header was handed
  straight to the SDK's C font loader `FSS_load_font`, which rejected it, so the CJK face was never registered.
- `runtime/font_hook.c` hooks the `FSS_load_font` thunk (the C entry every loader path passes through), keeps
  x0..x5, zeroes x6/x7 like the thunk, strips the shared font's 8-byte header and tail-jumps to the original
  `FSS_load_font_with_offset` PLT. The branch to the stub needs `adrp x16, page` **plus `add x16, x16, #page offset`**
  before `br x16` (the first attempt branched to the page base and hung the boot); the stub is a naked leaf with
  no stdio or 23 MB load (it runs on the font thread's small guest stack). The shared-font region is published
  to the hook from `GetSharedFontAddress`, never during the loader's early init.
- Hook is on by default now (`PLA_FONT_LOAD_HOOK=0` disables); the main-GOT LoadFont wrapper also strips the
  header as a fallback.

Verified: language title + warning render; beach dialogue renders Chinese; 30 fps, no faults. Next: the beach
still shows no characters in these frames (the dialogue speaker "？？？" is off-screen) — continue the intro and
identify the player/NPC draws in the beach pass.

## The missing characters: PLA skins on the GPU with transform feedback, which this port no-ops (same session)

With the text fixed the intro reaches **free movement** (HUD "L 移动 / L 冲刺" appears, dialogues render in
Chinese: "你还活着吗！？", speaker 拉苯, "就是为了深入了解宝可梦…"), but the **player model is invisible** — the
sand foreground where the third-person camera looks is empty, while terrain, the hut, the fire effect, the HUD
and all text render. The speech camera angles were not the explanation.

The NVN transform-feedback family is where the character goes: `runtime/../hle_nvn.c` implements
`BindTransformFeedbackBuffer(s)`, `Begin/End/Pause/ResumeTransformFeedback` and `DrawTransformFeedback` as
**accepted no-ops** (added during the first field-loader pass, 2026-10-31). nxnative, whose build renders the
player and NPCs on this game, **implements** feedback (`OP_TF_BIND` / `OP_TF_STATE`, a capture vertex entry
`main0_tf`; see its `docs/reference/rendering.md` "Transform feedback captured by a vertex entry point" and
`dm/gfx/nvn/hle_nvn.c`): a vertex-only draw between Begin/End captures the shader's outputs into a buffer that
later draws read as vertex streams. That is exactly GPU skinning: with the capture dropped, the streams stay
zero/absent and the skinned model collapses or is never drawn.

Our shader translator is the prebuilt `totk_xlate` (source: `d2r-mobile/dm/gfx/tools/dmsh_xlate/Program.cs`); its
reflection JSON has `msl_entry_kind` of fragment/vertex/kernel only — **no feedback/capture metadata**, so the
replay cannot emit a capture pass yet. Implementing this is the next feature: translator emits a capture vertex
entry + the feedback layout, `hle_nvn.c` records the TF buffers/state and the capture draws, `nvn_mtl.m` runs the
capture pipeline writing the varyings to the bound buffer and lets `<prim, buffer>` draws read it back.

Verified up to here: text everywhere (language, confirm, dialogues), free movement reached, 30 fps, no faults;
`/tmp/cc_1.png` is the free-movement frame with no player model.

## FIXED: the characters — transform feedback replayed through a generated capture entry (same session)

The player and the NPCs render. `final_14.png` / `txf_05..11.png` show the free-movement player on the beach
(HUD "L 移动 / L 冲刺"), `final_07.png` shows **Laventon behind Rowlet and Oshawott** (with Oshawott's water
splash), and `txf_04.png` the intro dialogue over the same scene ("这些逃走的宝可梦来到这里的，然后你就掉了
下来……"), text and all. The
previous section left the capture to the translator; the route taken instead needs **no translator change at
all** — the replay derives the capture entry from the MSL it already has, using the layout the game hands it.

**What the game does** (`TOTK_TF_LOG=1`, run to free movement — the evidence this feature was built on):
- one capture program, vertex shader `3374449f919e50dbf7dcf02f97ff84e425bebff3e2d7c3f603f6dced3228a674`; its
  control section declares three feedback streams — strides 12 / 4 / 4, one output each, location bytes
  0x20 / 0x21 / 0x22, widths 3 / 1 / 1;
- per mesh and per frame: `BindTransformFeedbackBuffer` ×3 → `Begin` → **one point draw** (`prim=0 first=0
  count=N inst=1`, `SetRasterizerDiscard` on) → `End`. The bound sizes are exactly `N*stride` (e.g. 4404 /
  1468 / 1468 for 367 vertices), so the three buffers are the skinned position xyz and the packed normal and
  tangent;
- `DrawTransformFeedback` is **never called** in PLA: the game binds the captured buffers as ordinary vertex
  streams and draws the model with the counts it already knows. The driver's 32-byte control buffer (the
  address handed to Begin/End) is written and never read back by the game.

**Layout vs. Metal varyings**: the location byte is `0x20 + the varying location`, not the varying word offset
Ryujinx's `ShaderDefinitions` works in — measured against the cached MSL of that shader, which declares
`m_37 [[user(locn0)]]`, `m_39 [[user(locn1)]]`, `m_41 [[user(locn2)]]`. With the word-offset reading the
entry would have written 0x21/0x22 as components 1 and 2 of location 0.

**Implementation** (this folder's `third_party/d2r_gfx/gfx/nvn`, no translator, no shader-cache change):
- `hle_nvn.c`: the seven NVN entry points now record `OP_TF_BIND` / `OP_TF_STATE` (what: begin/end/pause/
  resume + the control buffer) / `OP_DRAW_TF` instead of dropping the calls.
- `nvn_mtl.m tf_parse` reads the layout from the vertex program's control section: `+0x6dc` four u16 strides,
  `+0x6e8` output counts, per stream b the location bytes at `+0xdc+0x80b` and widths at `+0x4dc+0x80b`
  (interleaved from x on), the same words the Switch driver reads when the game begins the feedback.
- `tf_fn_for` builds the capture entry **once per shader** from its MSL: the original `main0` body with the
  last `return out;` replaced by stores into the streams, declared as
  `vertex void main0_tf(<main0's parameters>, device uint* dm_tf0..3 [[buffer(8..11)]], constant dm_tf_capture&
  dm_tf [[buffer(14)]])`, with `dm_linear = (iid - base_instance) * count + vid - first` and
  `dm_tfB[(offset[b] >> 2) + dm_linear * stride/4 + k] = as_type<uint>(out.<varying>.[xyz])`. Offsets and the
  captured vertex count live in the 32-byte record `{uint4 offset; first, count, base_instance, bound}`, so
  every capture draw gets its own block from the submit arena (Metal 4 does not order operations inside one
  encoder: a shared block would let a later draw's values reach an earlier draw).
- `tf_capture` runs the draw as a pass of its own: it ends the game's render pass, opens a 1×1 pass with **no
  attachments and rasterization disabled**, binds the game's vertex streams and argument block, the feedback
  streams at 8–11 and the record at 14, draws the same vertex range, then leaves the next game draw to reopen
  its pass. The capture is handled at the top of `draw_inner`, before the pass continuation / deferral /
  merge decisions, and the offsets advance by the captured vertices so `Begin/End/Pause/Resume` accumulate
  the way the driver does (the record is written back to the game's control buffer at End / Pause, taken back
  at Resume).
- `OP_DRAW_TF` draws `*(uint32_t *)(control + 16)` vertices from the bound streams (the count the driver would
  have left there); PLA never reaches it, but the semantics are implemented.

**Pitfalls (all found the hard way, all now covered):**
- **The capture pipeline must be keyed by the vertex layout, not just the shader.** Its vertex descriptor is
  built from the attribute formats and stream strides bound at that moment; the game binds different mesh
  layouts to the same skinning program. Caching by shader key alone reused the first descriptor, and the
  resulting out-of-range vertex fetch is a GPU fault: two `MTL4CommandQueueErrorDomain Code=1 "Discarded
  (victim of GPU error/recovery)"` events in ~20k frames, each within a frame of a capture, which the driver
  recovers from by discarding the command buffers (~2 frames of picture lost). Bisect: `TOTK_TF_MODE=3`
  (pass + pipeline, no stores) ran 10,800 frames with zero faults; with the layout-keyed pipeline the full
  capture ran 12,300 frames (past both fault frames) with zero. The stream writes are clamped to the pool's
  own page-rounded extent as a second bound (`RED_ONCE(84)` reports a stream declared larger than its pool,
  never seen in the runs).
- **`[[user(locn` is 11 characters**, not 10: the first parser took the `n` for the first digit and every output
  came out as location 0, so the generated entry wrote all three streams into one member. Caught offline by
  running the same text transform over the cached MSL and compiling the result with `xcrun -sdk macosx metal -c`
  before touching the game (the stores listed `out.m_37` for all three streams).
- **The entry must be appended to the whole translation**, not just `main0`'s body: the descriptor-set struct,
  the input struct and spirv-cross's helper functions all sit above it. `main0` stays untouched (the same
  variant is used for ordinary draws too; only the pass chooses the entry).
- Ending the game's pass and opening a bare one is only safe through the port's own helpers (`rp_end()` then
  `enc_r_begin`), which keep the copy / dispatch encoder and the render-stream state machine consistent; the
  next game draw reopens its pass with the usual (load) actions.
- `raster-discard=5700` in the old logs was these capture draws: the replay skipped every
  `SetRasterizerDiscard` draw. They now run through the capture entry (they rasterize nothing by
  construction).

**Verified**: player visible at free movement from several camera angles (`final_14/15/16.png`,
`conf_220.png`), Laventon and the two starters visible in the intro (`final_07.png`), title and dialogue text
unchanged (`final_04.png`), 30 fps (frame interval p99 33.6 ms) throughout. GPU faults: 30 error lines (two
events) in the 20,400-frame run with the pipeline cached by shader only (`logs/tf2_capture_ok.log`), **zero**
in the 19,500-frame run with the layout-keyed pipeline (`logs/tf_fixed_ok.log`) and zero in the 10,200-frame
run of the committed build (`logs/confirm_ok.log`); no TF red lines (the 3 red lines in the logs are the
pre-existing texture-copy / scissor reports). Capture can be switched off with `TOTK_TF_MODE=0` (2 = private
buffers, 3 = no stores: the bisect modes); `TOTK_TF_LOG=1` logs the binds, the layouts and every capture.

Next: the scene is overexposed (the same washed-out sky and sand appear in `/tmp/cc_1.png`, before this
change — the exposure / tone-mapping chain, not the characters); then the remaining intro beats and the first
battle.

## Overexposure: measured — the scene arrives bright, the tonemap is neutral at these settings (open)

The 3D picture is high key (sand and the player's shirt almost white, shadows lifted) while the UI and text in the
same frame are right; `/tmp/cc_1.png` shows it before the transform-feedback work, so it is not a regression from
that change. This section records what the frame actually contains, so the next session does not re-measure it.

**The post chain of a field frame** (`passdump 1` → `[pass]` records for F11588, 60 passes, 387 draws):
shadow maps (depth 1024²) → depth prepass (D32F) → the scene into `RG11B10Float` 1920x1080 (serial 3877) with
the depth as the depth attachment → a bright pass into `RGBA16Float` 960x540 → a nine-step minification chain
960x540 → 7x4 (the luminance the next frame's exposure adapts to) and the bloom down/up chain → the **tonemap**
`465dce9b2d…` writing the canvas (serial 3896, `RGBA8Unorm_sRGB`) → luminance 480x270 `R32F` (3899) → the
exposure update 1x1 `R16F` (3903) → UI passes into the same canvas → present (serial 1, `RGBA8Unorm`).

**The tonemap's brightness controls are neutral here.** `PLA_TONEMAP_LOG=1` prints the grading block (`c3`, 2472
bytes) the game hands it: the final multiplier `c3[154].y` = 1, the exposure thresholds `c3[114].yz` = 0 / 1, the
mid-tone scale `c3[98].zw` = 1 / 1, and the 1x1 exposure texture holds ≈ 1.0 (`/tmp/texdump/f390300.png`, texdump
of the frame). With exposure ≈ 1 and the thresholds at 0 and 1, the shader's exposure branches never fire
(`_213 < 0` false, `_213 > 1` false) and the output is the graded scene times 1.

**The scene itself is bright**: the scene target's dump averages 0.72 / 0.80 / 0.76 per channel
(`/tmp/texdump/f387700.png`, linear 8-bit conversion) and the finished canvas 208 / 227 / 215
(`f389600.png`). The UI on top of that canvas is correct, so the canvas encoding is right and the brightness
comes from the 3D content, not from the output mapping. Our language page (`/tmp/ho_1.png`) matches the reference
build's (`/tmp/ref1.png`) almost exactly, so the chain does reproduce the reference for a dark scene.

**Next, cheapest decisive check**: the reference build reaches the same beach
(`/Volumes/bit2t/switch/games/宝可梦 阿尔宙斯/Pokémon Legends - Arceus.app`); if its beach frame is visibly darker
than `/tmp/txf_05.png`, the difference is in the scene lighting chain (sun / sky / shadows and the material
shaders), not in post — compare the same pass breakpoints (scene target before the tonemap, the 0.72·0.80·0.76
average, and `c3[154].y`) against that build. Diagnostics added for this: `PLA_TONEMAP_LOG=1` (the block above)
and the input file's `texdump <path with %d>` (every colour target of the following frame as PNG, named by serial).

## FIXED: the outdoors washed out with no cast shadows — unwritten varying components are (0, 0, 0, 1) (same session)

The 3D picture now matches the reference build at the same beach: beige sand with its texture, green grass and
trees, blue sky, **and the player casts a shadow** (`/tmp/evidence_beach_after.png`). Same script, same frame
(screenshot 200 presses after the fall) with the seeding switched off: the old washed-out white
(`/tmp/evidence_beach_before.png`). At the cliff and in the grass the same run shows correct colour and
shading with a dialogue running (`/tmp/evidence_field_dialogue.png`, "阿爾 撿起了像是").

**Cause.** Maxwell reads the components of a varying the vertex shader never writes as **(0, 0, 0, 1)**. The
translator's MSL starts the output struct as all zeros (`main0_out out = {};`), so every output the body only
partly writes keeps w = 0. In PLA's cache **308 of 740 float4 vertex outputs are partial** (118 miss wz, 94 miss w,
74 miss wyz, ...). The surface shaders divide a world position by that w to compute the shadow-map lookup; with
w = 0 the compare coordinates are infinite, every shadow sample misses, and the whole outdoors is lit without
shadowing -- bright and flat, while the UI (same canvas) stays right. This is nxnative's
`docs/reference/shaders.md` "Unwritten components of a written output start as (0, 0, 0, 1)" (found on Link's
Awakening, symptom "outdoors dim and nothing casts a sun shadow"; their offline `vs_init` does the same seeding).

**Fix** (host-side; no translator, no shader-cache or chain change): `msl_init_vertex_outputs` seeds every
float4 user varying with `float4(0, 0, 0, 1)` right after `main0_out out = {};`, letting the translated body
overwrite what it writes. Applied where a vertex library is compiled -- at load (`shader_load`) and in the derived
vertex variants (`vs_fn_for`, including the varyings it adds for the fragment stage). `TOTK_VS_NOINIT=1` switches
it off for A/B.

**Verified.** A/B on one script to the same frame: before `/tmp/evidence_beach_before.png` (washed out, no
shadow), after `/tmp/evidence_beach_after.png` (the reference build's look, with the player's shadow); 30 fps
(frame interval p99 34.0 ms), zero GPU faults, only the 3 pre-existing red lines (scissor / texture copy forms /
pre-arranged texture), text and characters unchanged.

**Notes.** The earlier measurement stands (`bc3ae95b`): the tonemap and the 1x1 exposure feedback are neutral at
these settings -- the brightness was the missing shadowing, not the post chain. Cost: every vertex shader's MSL
now differs from the text in the shader cache, so the prebuilt shader libraries (named by the MSL's SHA-256) no
longer match for vertex shaders and are compiled at load; the warm-up is a little longer, the run is otherwise
unchanged.

## Audio: PLA plays through AudioOut, and its output is still never opened (open — measured this session)

No sound yet, but the path is now known and the host side of it is implemented. What was measured:

**PLA does not use the audio renderer.** `main` imports exactly thirteen `nn::audio` functions and all of them are the
**AudioOut (audout) family**: `OpenDefaultAudioOut`, `AppendAudioOutBuffer`, `SetAudioOutBufferInfo`,
`GetAudioOutBufferDataPointer`, `GetReleasedAudioOutBuffer`, `StartAudioOut`, `StopAudioOut`, `CloseAudioOut`,
`GetAudioOutState`, `GetAudioOutChannelCount`, `InitializeAudioOutParameter`, plus the device queries
`GetActiveAudioDeviceName`, `GetActiveAudioDeviceChannelCountForOutput` and
`AcquireAudioDeviceNotificationForOutput`. No `OpenAudioRenderer`, no `nn::audio::server` use. `audren:u` is opened
four times, but only for the **audio device** service (`GetAudioDeviceServiceWithRevisionInfo`, command 4), and the
`audout:u` IPC service — the one `runtime/audio_out.c` already implements with Core Audio behind it — is **never
opened at all**, in any run so far (language page through the beach and the intro).

**What was implemented** (host side, all in this folder's `runtime/`):
- `audio_device_sdk.c`: the SDK's audio-device API answered by the host instead of the SDK's client —
  `List/GetActiveAudioDeviceName` (a name from `PLA_AUDIO_DEVNAME`, default `AudioTvOutput`),
  `GetActiveAudioDeviceChannelCountForOutput` (2), and `AcquireAudioDeviceNotificationForOutput` (an event bound with
  `hle_bind_system_event`, **auto-clear and signalled once**, then `0x499` on a second acquisition). The initial
  signal matters: without it the game parks on that event; with it the game leaves the wait (it then queries the name
  once more and stops).
- `audio_sdk.c`: the whole AudioOut API over the existing core in `audio_out.c` (`au_core_open/append/released/...`,
  Core Audio or the silent sink): open by parameter (default 48 kHz stereo), append (PCM16, 5.1 downmix as before),
  released buffers, start/stop/close/state/channel count, and the buffer descriptor as the public 0x28-byte
  `AudioOutBuffer`. The audout:u IPC service stays for other paths.
- `audio_mixer.c`: the audren **device** service now returns a Result with the notification event (it returned the
  event alone; the SDK client read an unspecified value), and a second acquisition answers `0x499`.
- Diagnostics: `PLA_AUDIO_TRACE=1` logs the device calls, `PLA_AUDIO_CALLER=1` logs their guest call sites as
  `module+offset` (`main+0x…`).

**Where it stops.** With the host serving all of the above the game still never calls `OpenDefaultAudioOut`. The
call sites of the device queries are `main+0x277e5d8` / `main+0x2780444` and `main+0x2e33364` / `main+0x2e33388`;
the code at `main+0x2780418` **parses the active device name into a kind** — it compares the name it gets against
`AudioStereoJackOutput`, `AudioTvOutput`, `AudioUsbDeviceOutput` and the built-in speaker name and picks 0..3 — and
`main+0x2e33358` is a one-shot "device available" helper (name query, then the notification, then a flag). Both
return normally; the output open is somewhere else and is not reached.

**Probe results since (same session).** The gates are `adrp x16; ldr x17,[x16,#off]; add x16,x16,#off; br x17` at
`main+0x32a4d50` (notification), `+0x32a4d60` (device name), `+0x32a5530..0x32a5610` (the whole AudioOut family;
`OpenDefaultAudioOut` = `+0x32a5580`). Only one `bl` reaches the open gate: `main+0x2e35bac`, inside the sink's init
method at **`main+0x2e35b14`** (`InitializeAudioOutParameter` → sample rate from `[this+0x24]` → channel count 2 or 6
→ `OpenDefaultAudioOut` → `GetAudioOutChannelCount` → `Append` → `Start`; its close method is `main+0x2e35df0` at
vtable `main+0x41ad2e0`). A code probe on that entry (`PLA_AUDIO_SINK_PROBE=1`, the same `adrp/add/br` + prologue
replay as the font hook) shows it is **never called** — not at the language screen and not in the intro after the fall
(`F9600`), where the game does read the three BGM tables. The sound engine therefore never starts, and `OpenDefaultAudioOut`
is never reached at all.

**Where the sound init stops.** `PLA_AUDIO_FS_LOG=1` logs every open under `bin/sound`: at boot the game opens exactly
`bin/sound/table/bgm_layer.bin`, `bgm_transition_rule.bin` and `bgm_transition_override.bin` (all three are in the RomFS
index, sizes 940 / 1348 / 11464) — and **never a Wwise bank** (`bin/sound/NX64/wwise/bank/*.bnk`, `Default.pck` 674 MB,
all present). `PLA_AUDIO_FS_LOG=all` over the whole boot (1,811 opens) confirms it: no `.bnk`, no `.pck`, no path
with `wwise` in it is opened — only `bin/misc/app_config/sound_config.bin`, the three BGM tables and the field
placement `sound_event.bin` parameter files. So the stop is after the BGM tables and before the engine's bank load,
i.e. between the game's sound manager and `AK::SoundEngine::Init`, not in the file system and not in
`OpenDefaultAudioOut`.

**The sound engine does start — and is then suspended by the game.** `PLA_THREAD_NAMES=1` prints the names the game
gives its threads: `JobThread1/2`, `FileManager`, `FsDecodeThread` ×3, `RenderWorker` ×3, then Wwise's
`AK::IOThread`, `AK::BankManager`, **`AK Suspended`**, `AK::OpusDecodingThread` and `AK::EventManager`. So
`AK::SoundEngine::Init` ran (its threads exist), and the engine was put into Wwise's *suspended* state right after:
the guest chain that names `AK Suspended` (`PLA_THREAD_NAMES=2`) is
`main+0x2dc90b0` (Wwise's suspend: `InitializeEvent` at `0x430a7a0`, then a thread named from the string at
`0x38354dd`, body `main+0x2dc91b8` which loops on `WaitEvent(0x430a7a0)` and tests the flags at `0x430a798` /
`0x430a7c8`) ← `main+0x2e332cc` (the game's sound init) ← `main+0x2d75810` ← … ← `nnMain`. A suspended engine never
creates the sink (`main+0x2e35b14`), never opens `AudioOut` and never loads a bank — which is exactly what the file
log shows. The `AudioOut`/device API on the host side is fully implemented and waiting.

**The suspend is an idle timeout, and nothing ever resumes.** Wwise's suspend/resume toggle is `main+0x2dc9278`
(w0: 1 = suspend, 0 = resume; it stores the flag at `0x430a5b8` and signals the `AK Suspended` thread's event
`0x430a7a0`). It has exactly three call sites in the whole program — `main+0x2d7e998`, `+0x2dc90c0`, `+0x2dc9bcc` —
and all three pass **w0 = 1**: the game never asks for a resume. A probe on the toggle (`PLA_AUDIO_SR_PROBE=1`) shows
one call at boot, suspend. The first call site is the sound manager's idle timeout in its tick (`main+0x2d7e250`:
"no audio activity for longer than `[0x430a328]`" → suspend), i.e. the suspend is a *symptom* of nothing ever having
played. Forcing that call to resume instead (`PLA_AUDIO_FORCE_RESUME=1` patches `main+0x2d7e990` to `mov w0, #0`) does
not bring the device up either, so the engine never had one.

**What is left**: the game's sound manager never constructs its audio device object (the sink at `main+0x2e35b14`,
the only caller of `OpenDefaultAudioOut`), so `PostEvent` has no device, nothing plays, no bank is requested and the
idle timeout suspends what is left. The next probe is that manager's own init/state machine (`main+0x2e33xxx`, the
code that reads `sound_config.bin` and the three BGM tables at boot) — a trampoline probe on its steps shows which
condition it stops at; the host side (device API, AudioOut API, Core Audio) is complete and waiting.

## Audio: sound works — the boot suspend blocked the sound system, and the SDK AudioOut adapter had three bugs (fixed — measured this session)

The previous entry's picture was right about the chain but wrong about the suspend: the "AK Suspended" thread is
created by the chain's LAST step (main+0x2dc903c) as part of normal init, and the call the SR probe caught
(main+0x2dc90c0) is that same step suspending the engine at boot because the boot gate `[0x430a600]` is set
(= 3) — not an idle timeout (the idle path at main+0x2d7e998 never fired; `[0x430a248]`, the tick's command
count, stays 0 the whole session). The game never passes 0 (resume) anywhere: all three toggle call sites are
hardwired to 1, and the resume that does happen comes only through Wwise's own event processing — which needs a
loaded bank, and no bank is ever requested while the engine is suspended. Code probes on this handshake perturb
its timing and hang the boot; a zero-perturbation memory poller (`PLA_AUDIO_STATE_POLL=1`) is what sees it: the
engine is suspended at t≈2 s and never leaves, the tick thread ("AK::EventManager", body main+0x2e330d4, fed
from `[0x430a128]`) parks in its suspended branch, and the sink at main+0x2e35b14 is never constructed.

**The fix (on by default, `PLA_AUDIO_SUSPEND=1` restores the game's behaviour):** boot the engine unsuspended —
patch the no-device branch of the toggle to store 0 (main+0x2dc92b0 `strb wzr`, was `cset w8, (w19==1)`) and the
boot call to pass 0 (main+0x2dc90b8 `mov w0, #0`). suspend_req then reads 0, so the tick's idle path also never
re-suspends after the AK thread dies. With that, the game's sound manager sees the engine available during its
own init, marks audio usable and posts requests: at t≈18 s (title area) the engine resumed itself and the sink
opened — `OpenDefaultAudioOut(48000 Hz, 2ch)` at last, ~2 800 AudioOut API calls following.

**With the engine up, the sink still starved, for three adapter bugs (all fixed):**
- `AppendAudioOutBuffer` is the Auto form: the sink prepares each buffer with `SetAudioOutBufferInfo` (measured
  `(4096, 4096)` per one-page buffer) and hands over the whole buffer, so a `data_size` of 0 means "use
  capacity". Without the fallback every append was refused as "queue full" (the core treats 0 bytes as
  refused), the sink starved and the game tore it down and reopened it forever.
- `GetAudioOutBufferDataPointer` must return `sample_buffer + data_offset`: the sink writes its PCM there and
  Append reads the same window back.
- `au_core_append` never handed buffers to the AudioQueue worker at all (the dispatch line was lost while the
  worker was being wired in; the audout:u IPC path had it, the SDK path did not) — found by running the game's
  own core path from a plain host thread at boot (`PLA_AUDIO_SELFTEST2=1`): appended, started, three empty
  released polls, done.

**The queue itself lives on a dedicated host worker thread** (all AudioQueue calls — open/enqueue/start/pause/
reset/volume/dispose — are posted to it, PCM is copied first because the caller's memory is guest memory).
`GetReleasedAudioOutBuffer` waits up to 60 ms only while a fresh sink is priming (queued buffers, none played,
first Start cycle); in steady state it is poll-only as on hardware — an always-on wait ran the game at 1.7 s
per frame, none at all let the game declare the sink dead. `audio_out_close` no longer frees the AudioOut
(~1 KiB per sink generation): a disposed AudioQueue can still deliver a played callback, and one that landed on
the freed struct ended the run on event handle 0. The handle table grew 4096 → 16384: the engine idles its
output at silent scenes (~8 s suspend / request-driven resume) and every sink rebuild binds two notification
events, which filled 4096 handles in about four minutes of menu silence.

**Where this stands:** a 280 s boot with input reaches frame ~5 800 at ~21 fps with zero faults, the sink fed
and drained continuously (48 MB of PCM appended), no handle exhaustion. Still open: the game cycles its sink
(~17 rebuilds/s, each cycle appends four 4096-byte buffers, polls, collects its releases, then re-initialises —
after one fully successful cycle `OpenDefaultAudioOut` stops being called while `InitializeAudioOutParameter`
spams, i.e. the game's own retry gives up between Initialise and Open for a reason not yet identified) — that
churn costs the frame rate and is the next target; and no musical scene has been reached yet (the name-entry
flow needs the swkbd script), so real BGM is verified only as far as the pipeline, not by ear.

## Boot shows a black window at ~30 fps — pre-existing, not the audio work (bisected this session)

With the audio fixes in (and equally without them: checked out 8d9eab98 and re-ran), the window opens and the
game advances frames at ~30 fps but the screen stays black: zero texture uploads, zero new pipelines, zero
`[RED]` lines, `GetLoadState` stops after 8 queries (fs_reads 190), and at t=30 s (`TOTK_DUMP_AFTER=30`) the
main thread sits in an `nn::os` condition-variable wait at main+0x296d9a4 <- main+0x296b05c <- nnMain, with the
loader-side thread at main+0x29497b4 -> main+0x2ac9bb8 — the loader/movie handshake of earlier sessions. The
game is parked in a scene that renders nothing, before the language screen. Separately, one boot in this
session died straight away with `FAIL: reserve guest stack, heap and alias regions below 2^39` — the macOS 27
random ~24 GiB reservation inside the alias window (runtime/services.c) is flaky per boot; a retry booted.

The strongest lead for the next session: the reference implementation (nxnative `dm/games/01001F5010DFA000`,
same `sdk` build b39add63) **does not load subsdk0 at all** ("the movie module, which the profile does not
load: main takes nothing from it the host does not supply") and runs PLA from first boot to the beach headless;
this port loads subsdk0 (boot step 2/4) and carries the whole movie-wait bypass history. Compare the boot flow
without subsdk0, and re-check the language screen against the OS update (27.2, build 26B123) — nxnative's
services.c already documents macOS 27 moving system reservations into the guest window.

**Explained an hour later: the black window is deliberate.** `nogpu()` (nvn_mtl.m) — full-game runs submitted
GPU work and **kernel-panicked macOS four times (IOGPUFamily, identical registers)**, so the game never submits
unless `TOTK_GPU_ALLOW_SUBMIT=1`; nothing is shown, the presented handler never fires, the boot overlay never
hides, and everything else this session observed (frames ticking, zero uploads, zero RED lines, the game
reaching the language-select state and cycling its sink) is the guest running with an empty GPU pipeline.
The audio fixes stand; the picture — and any by-ear audio check — waits on the IOGPUFamily panic being fixed
(or worked around) with submission re-enabled. The panic trail starts at four identical IOGPUFamily crashes on
this M5 / macOS 27.2 machine; nxnative's host submits PLA's work on an M6 without panicking, so the fault is
most likely in this fork's d2r_gfx encoding, not the game.

## Picture restored (GPU submission re-enabled, approved bounded run) — audio still silent: banks never load (next target)

A user-approved bounded run with `TOTK_GPU_ALLOW_SUBMIT=1` — the Odyssey playbook for this exact class of
problem (same machine, same copied renderer, same IOGPUFamily history; its bounded runs never crashed) —
booted PLA with the picture back: `first presented frame` at 1.6 s, ~22 fps with real GPU work (1.4 ms/frame),
no panic, no new RED lines beyond the known cosmetic three. The user confirmed the game picture on screen
(the language-select screen; the FS log shows the game loading `bin/appli/lansel` UI resources there).

Sound is still silent: 180 s with input delivered all 40 key presses (the input file does NOT loop — each line
plays once; the earlier two-press scripts fired at t≈0 and were lost at boot), the sink pumped PCM the whole
time, but the dump is pure silence and **not a single .bnk/.pck open** — the game never calls LoadBank. The
per-frame command queue the tick drains stays empty (`[0x430a240]` = 0, never written — the teardown path at
main+0x2d75940 frees these queues when set; the allocator side was not found by direct-store scans and is
probably a register-offset loop). The tick's per-frame helper main+0x2dc98a8 is Wwise's ProcessAudioControl:
it walks the registered-device list ([0x430a7e0]), reads the suspended flag and drives the sink open/close
churn (~17 rebuilds/s at silent scenes) — both the churn and the bank-load gate live in this function family
and are the next session's target. Runbook reminder: `PLA_AUDIO_CYCLE_LOG=2000-2120` prints whole sink
lifecycles uncapped, `TOTK_DUMP_AFTER=<s>` prints every guest thread's wait chain, and the input file needs
enough lines to cover the whole window (or lines with holds up to 5 000 ms).

## Bank-gate investigation round two: every gate passes, the paths are built, yet LoadBank never fires

Mapped the whole AudioOut import-gate block (JMPREL): OpenDefault 0x32a5580, ChannelCount 0x32a5590,
SetBufInfo 0x32a55a0, GetState 0x32a55b0, Stop 0x32a55c0, Close 0x32a55d0, DataPtr 0x32a55e0, Append
0x32a55f0, GetReleased 0x32a5600, Start 0x32a5610; device gates notification 0x32a4d50 / name 0x32a4d60.
The sink vtable (0x41ad2e0, 32 slots via RELA): +0x28 init (0x2e35b14), +0 close (0x2e35df0 — which itself
does GetState→Stop if started, then Close), +0x60 fill-with-mix (0x2e35ec8: GetAudioOutBufferDataPointer +
Append, converting the float mix at [sink+0x28] to s16), +0x68 fill-with-silence. The sink faithfully renders
the game's mix — the mix is silent because Wwise has no events, because banks never load.

With `TOTK_GPU_ALLOW_SUBMIT=1` and the flow driven to the opening cutscene (scripted: language confirm →
kbtype/kbenter name → prompts), the user confirms the picture advances through the cutscene but it is silent
end to end. The bank-path gate chain all passes: the registry walk (0x84cba4 from main+0x277e5ec) returns 1,
the gate object's ready byte ([0x4279cd0] → deref) sets bit0 at t≈2 s, the bank path strings are built —
and still zero .bnk/.pck opens all run. The registered device's state word [dev+0x160] flaps 1↔0 every
10–20 s (running↔stopped, each transition submitted as an async job through vtable 0x41ab310 +0x38 — the job
system works; [dev+0x138] holds the in-flight job), i.e. the engine's own idle suspend/resume cycle at a
silent scene, benign. All AK threads exist and idle normally; nothing spins.

Next-session leads: (1) PLA's runtime lacks a guest `svcOutputDebugString` logger — Wwise/AK reports its own
errors there (totk/nxnative's DM_DBG); add it and read what AK says about Init. (2) The bank consumer after
the path strings are built (the tail of main+0x277e55c–0x277ed00; bl 0xaef9ac at 0x277e7b0) — whether banks
load per-scene from the game's audio director and what state gates it. (3) Banks may come from memory (the
GFPAK archives, 14 `bin/archive` opens) rather than loose .bnk files — a load-from-memory would show no file
opens at all, in which case the silence means no events are posted, not that banks are missing. (4) The tick
command queues ([0x430a240]/[0x430a250]) still never allocate; their allocator is not a direct store and the
teardown (main+0x2d75940) is the only direct site — find it via the register-offset form.

## Audio: the output device works — AcquireAudioDeviceNotificationForOutput was the blocker (same session)

**The sink now opens.** The fix was the answer to the device-change notification: the first acquisition binds an
auto-clear event **and signals it once**, every later acquisition binds its own event the same way (no 0x499), and
the audren device service returns a proper Result. With that, the whole chain came alive in the field:

`[audprobe] audio sink init reached (#1)` → `InitializeAudioOutParameter` → `OpenDefaultAudioOut` → **`[totk-audio]
AudioOut open 48000 Hz x2 PCM16 (SDK API)`** → `Start` → `Released` → ... → `Stop` → `Close` → re-open. The game
drives the device continuously: 24,600 appends (~100 MB) over a session that reaches free movement and walks the
field (screenshot `/tmp/walkfield_360.png`), 30 fps, zero faults.

**What was still blocking it before this session's fix** (for the record): the ctor of the audio device object
(`main+0x2e33334`) stores its ready flag `[obj+0x10c]` only when `AcquireAudioDeviceNotificationForOutput` returns
Result 0 (or the one-shot flag `[0x430b2d0]` says it already succeeded); the manager tick (`main+0x2d7e438`) checks
that flag every frame and only then calls the device. A wrong notification answer kept the flag 0 forever, the sink
never opened, no bank was ever requested, and the engine's idle timeout suspended audio — every symptom we had.

**Still silent, now measured precisely** (`PLA_AUDIO_PCM_LOG=1`, `PLA_AUDIO_FS_LOG=all`):
- every append is **all zeros** (`[pcmfed] append #24600 bytes=100761600 nonzero=0/24600`);
- the only bank ever requested is `ENV_BASE_AREA00.bnk` — **32 bytes, an empty placeholder** — loaded once in the
  field through the game's low-level IO (`main+0x28386ac`, romfs open OK);
- `Init.bnk`, `Common.bnk`, `BGM.bnk`, `Default.pck` and every other bank/media file are **never opened at all**;
- Wwise's suspend/resume toggle (`main+0x2dc9278`, probed) fires once at boot (w0=0) and then repeatedly with
  w0=1 from the idle-timeout site `main+0x2dc9bd0` — the engine suspends itself because nothing plays. Forcing
  resume (`PLA_AUDIO_FORCE_RESUME=1`) changes nothing: the suspension is a symptom. So is the "AK Suspended" thread.

**Conclusion**: the remaining silence is not in the host's device/IO layer — it is that the game's audio director
never issues a single LoadBank (no Init bank = no mixer busses = every event silently fails). The next probe target
is the game's LoadBank entry: the bank-name resolver/FNV table lives around `main+0x277e2fc`..`main+0x277e9c0`
("Init.bnk" string referenced at `main+0x277e670`), called through function pointers; probing there will show
whether LoadBank is ever called and with what name — and if it is, why the open never follows.

**Probe infrastructure added this session** (all env-gated, default off): `PLA_AUDIO_SINK_PROBE` / `PLA_AUDIO_SR_PROBE`
(code trampolines on the sink init and the Wwise toggle, prologue-replayed), `PLA_AUDIO_DEV_PROBE` (device vtable
methods), `PLA_AUDIO_FORCE_RESUME`, `PLA_AUDIO_TRACE`, `PLA_AUDIO_CALLER`, `PLA_AUDIO_FS_LOG[=all]` (every sound/bank
file open with its guest call site `module+offset` and romfs/host result), `PLA_AUDIO_PCM_LOG` (append count + nonzero
sample), `PLA_THREAD_NAMES[=2]` (guest thread names and the chain that named them), `PLA_THREAD_CHAIN`.
Probe bugs found and fixed the hard way: a trampoline that replays only part of a prologue must not skip the frame
record push (`stp x29,x30`) — the SR probe crashed the toggle with pc=0 at boot until it replayed the full prologue.

## Audio: the bank-open call chain, and the shape of what remains (same session)

`PLA_AUDIO_CHAIN=1` captured the full guest chain of the one bank that does load (the audio thread processing a
queued request):

```
main+0x28386ac  nn::fs::OpenFile gate
main+0x28384e8  the low-level IO Open wrapper
main+0x2835874  the bank path resolver (localized / base fallback)
main+0x2d65db0  Wwise IO dispatch
main+0x2d906e4 / +0x2d90718  sync / async open
main+0x2d8f570  Wwise IO manager
main+0x2e0e318  the audio thread's loop
```

Right after it, the sink opens the AudioOut device — the ENV placeholder load is part of the device startup. So the
low-level IO path works end to end; what never happens is the **game-side `LoadBank` call** for Init.bnk /
Common.bnk / BGM.bnk (and the Default.pck package open). Nothing upstream of the IO is stuck: the audio thread
processes requests, the device opens and is driven every frame, the file system answers every open. The silence is
the game's audio director never issuing a load — its trigger is still unidentified. Candidates for the next session:
the audio director's start gate in the sound init chain (`main+0x2e0f6e4` region — note 0x2e0f764 next to it is the
teardown), the movie-thread state that used to spin at boot, or a game-state flag the BGM director polls from the
bgm tables it read at boot.

## Audio: how the game names its sound files (same session, for the next hunt)

The Wwise IO resolver at `main+0x2838720` builds file names from **numeric IDs** — the format strings at
`main+0x38c145f` (`"%u.wem"`) and `main+0x38b799e` (`"%u.bnk"`) — while the observed `ENV_BASE_AREA00.bnk` open goes
through a friendly-name path. The RomFS has **no loose `.wem` and no numeric-`.bnk` files** (36 files under
`/sound/` total: the named banks + tables), so all streamed media lives in `Default.pck` (674 MB, the Wwise file
package — whose name string appears nowhere in `main`; it is built at run time or opened via Wwise's package API).
The named banks carry embedded media (`Common.bnk` 15 MB, `PC.bnk` 8.3 MB, `PM_NORMAL.bnk` 21.7 MB), so basic sound
only needs the named banks to load.

That is the state of the hunt: the host IO answers every open; the game never issues a load. The remaining probe
target is the game-side caller that should issue `LoadBank` / `LoadFilePackage` once — the audio director's start
gate. The instrumentation to observe it is all in place (`PLA_AUDIO_FS_LOG=all` catches any open; `PLA_AUDIO_PCM_LOG`
catches any real audio the moment it flows).

## Audio: session wrap — device chain fixed, bank-load gate identified as the remaining hunt (same session)

Probe results closing this session: the sink init runs and opens the AudioOut at boot (`AudioOut open 48000 Hz x2
PCM16 (SDK API)`); the appends flow continuously (all zeros); the **device object's tick-driven methods
(dev.m1 / dev.m3.getkind, `PLA_AUDIO_DEV_PROBE=1`) never fire** — the manager tick's device path is not the append
path (appends come through the sink object's own methods). The sink factory (0x2e35840) never runs either — the
sink is the static singleton at `0x430b2d8`, constructed once at boot.

The remaining silence is one level up from everything the host touches: **the game's audio director never issues a
LoadBank** (Init.bnk / Common.bnk / BGM.bnk — never requested; Default.pck never opened; no loose .wem files exist).
When a bank load does reach the IO (the ENV placeholder), the whole path works. The next session's single target:
find the game-side LoadBank call site and its start gate. The most promising instruments: probe the bank-name
hash-lookup function (the FNV-1a code around `main+0x277e2fc`..`0x277e9c0`, "Init.bnk" referenced at
`main+0x277e670`; reached through a command dispatcher at `main+0x277c5a0`..`0x277c6bc`) and the audio thread's
request queue (`main+0x2e0e318` loop, proven to process whatever is queued).

## Audio: the LoadBank call site, mapped statically — one wrapper, one gated caller (session)

Game-side reverse engineering of the LoadBank hunt, done statically against `PLA_build/analysis/main.elf`
(module offsets below; the Ghidra project used is disposable — rebuild with `analyzeHeadless <proj> pla_main
-import main.elf -noanalysis`, script `nxnative/dm/tools/ghidra/Decompile.java`, Ghidra address = module+0x100000;
function boundaries from the ELF's `eh_frame_hdr` give 158,886 functions; a raw BL scan over .text gives the
call-graph; RELA addends give vtable slots). All offsets double-checked by hand against decompiles — beware the
two System-9-class mistakes both hit this session: Ghidra addresses are module+0x100000 (a global seen as
0x440a148 in decompiles is really `0x430a148`), and Ghidra's `func_0x02Axxxxx` names are Ghidra addresses too.

**The engine never loads Init.bnk.** `AK::SoundEngine::Init` is `0x2d74ee0`: it saves the 15 settings pointers,
creates every Wwise singleton (`0x2e75160` — stream manager 0x430a120, low-level IO block 0x430a160, the bank
manager **0x430a148**, event manager 0x430a140, sound core 0x430a130, ...), runs `0x2e75800` (subsystem start)
and `0x2eb3268` (platform init), then fires the init-callback array at 0x430a2d0. **None of these — checked in
all four bodies — contains a single bank load.** There is no automatic Init bank in this Wwise build; loading
Init.bnk is entirely the game's job, and the game never does it (see below).

**The sound bootstrap runs unconditionally from nnMain.** `nnMain` (0x2779f70) → game bootstrap 0x27812dc →
sound-system init `0x277c530` (the "command dispatcher at 0x277c5a0..0x277c6bc" of the previous session is just
call sites inside this giant init) → config/bank-name loader `0x277e5b0` (the "hash-lookup function" region is
`0x277dcf4` (a separate FNV/registry helper) plus this function). `0x277e5b0` passes its one gate (the registry
walk `0x84cba4` — measured 1 last session), builds the bank base path `bin/sound/NX64/wwise/bank/`, builds the
`Init.bnk` path, looks up `bank_names` and `package_names` (two 32-entry tables keyed `"%d"` in `sound_config`),
registers them in the sound registry (helper `0xaef9ac` with the interned-name struct {u64 hash, char* name,
u32 len} — `sound_config` hash `0x56237091ed6bee53`), calls the engine-start `0x28104e8` (0x18e4 bytes; sets the
Wwise language to "English(US)" via vtable+0x30, builds the engine's bank bookkeeping — **but issues no bank
load either**), then posts `SET_STATE_GAME_AWAKE` (wrapper 0x2813338) and sets RTPC `SPEAKER_MODE`=1.0
(wrapper 0x2813898), then reads the three BGM tables. So every gate the previous session probed sits *here*, and
the chains this function drives are alive — the missing load is downstream, in the wrappers.

**The game's Wwise API surface (all take the interned-name struct):**
- **LoadBank** sync/async = wrapper `0x2812fb8` → engine API object (`*(*(gameAudio+0x158)+0x60)`) vtable+0x100 /
  +0x108 (async takes completion cb `0x280f2b0`), after an "already loaded?" check `0x28180dc`.
  **It has exactly one direct BL caller in the whole binary: `0x853f1c`**, the Pokémon voice bank loader
  (builds the literal `PM_N_487.bnk` interned name), gated on `[[component+0xa0]+0x54] == 1` — the
  `gamesys::audio::SoundController` state word must already be 1.
- **UnloadBank** = `0x2811dcc` → vtable+0xe0/0xe8 (callers: PM voice `0x854984`, the huge field sound system
  `0x1877a4c` (11k lines, vtable-called, also calls the already-loaded check), and `0x1ddde80`).
- **PostEvent** = `0x9b9db4` → vtable+0xc0; when PostEvent returns 0 (event unknown — which it will be with no
  banks loaded) the wrapper silently skips the bookkeeping. 7 call sites.
- **SetState** = `0x2813338` — 121 call sites in 85 game functions; **SetRTPC** = `0x2813898` — 14 sites. The
  game talks to the engine constantly; the engine just has no banks to resolve anything against.
- Engine-side bank-slot management: acquire/release trio `0x2e19760` / `0x2e197b4` / `0x2e19998` and the bank
  array Term path `0x2e30b20`; the sync low-level IO entry is `0x2d96d40` (vtable slot 0x41a7de8).

**The IO/bank-manager floor is fully identified** (all pointer/vtable-driven): the "AK::BankManager" thread
(creator `0x2e0dfcc`) runs the IO loop `0x2e0e21c` over a 0x50-byte request-node queue at manager+0x1a0;
dispatcher `0x2d8f2c8` (request types 0=open, 1–8, 10); the open path `0x2d90504` contains the BKHD version
check (`0x2d904b0`, the "Load bank failed : incompatible bank version" string); the file-name resolver
`0x28355f0` FNV-1a-64s the lowercased name, binary-searches the package tables (Default.pck's registry, empty
because the package never opens), and falls back to a direct open by name (`0x2838428`) — which is exactly how
the loose `ENV_BASE_AREA00.bnk` got in, and how a LoadBank(Init.bnk) *would* get in (Init.bnk is a loose RomFS
file), so the absence of its open is conclusive: **the load is never requested, not mis-pathed.**

**Where this leaves the hunt.** Everything between nnMain and the file system is now mapped, and the only bank
load the game ever issues is gated on the SoundController state word (`+0x54`) that never reaches 1 on the port.
Two concrete next probes: (1) decompile the SoundController class (RTTI name string at 0x38915f8; its methods
are in the 0x85xxxx/0x8axxxx neighborhood) and find the writer of `+0x54` — the state machine that must advance
to 1 is the audio director's start gate for the one proven load path; (2) add a runtime probe on the engine
vtable+0xc0 (PostEvent) and +0x100/+0x108 (LoadBank) entries — a code trampoline like PLA_AUDIO_SINK_PROBE — to
log every event/state/load the game pushes and the AKRESULTs; together with the still-missing guest
`svcOutputDebugString` logger (lead #1, AK reports its own errors there) this will show whether any event ever
leaves the game and what the engine says about the ones that do.

**Addendum, same session — the gate machine found.** The SoundController state word `+0x54` is written by a
per-frame component updater: vtable-called `0x8413a0` (0x68 bytes) → `0x8417a8`. It early-outs once `+0x54` is
non-zero for the frame, reads the app state as `*(*(*(this->8) + 0x98) + 0x58) -> +0x10d4` (the game-state
manager's current state), and sets **`+0x54 = 1` (audio allowed) only for app states 1 and 4**; state 3 sets
`+0x54 = 3` with a result query (`0x295e488`/`0x2941224`, stores `result==2` into +0x228 and `result!=2` into
+0x58 — the loading path), any other state sets `+0x54 = 2` (audio off). So bank loads are legal only while the
game is actually in-game, and the one proven LoadBank path additionally needs a per-event trigger. Next session's
dynamic probes stand (engine vtable+0xc0/+0x100/+0x108 trampolines, `svcOutputDebugString` logger, watch
`[app+0x58]->+0x10d4` during boot), plus: check whether the port's game state ever reaches 1/4 (the field walk
reached the opening cutscene, so it should — if it does, the missing loads are event/trigger-driven and the
PostEvent probe becomes the primary instrument).

## Audio: the API probes, and what the running game actually does (session)

Built a full runtime instrument set (`PLA_AUDIO_API_PROBE=1`, `runtime/audio_sdk.c`): entry trampolines with
prologue replay on 14 sites — the three game-side PostEvent wrappers (`0x2813338`, `0x9b9db4`, `0x281356c`,
`0x2813288`), the two game-side LoadBank wrappers (`0x2812fb8` engine vtable+0x100/+0x108, `0x2813134`/`0x28131e4`
vtable+0xf0/+0xf8), the four engine-side impls (`0x2dcb224`, `0x2dcb9b8`, `0x2dcb558`, `0x2dcb664`), the gfl bank
loaders (`0x8dbe34` async, `0x8df3bc` sync), the SoundController gate updater (`0x8417a8`) and two small wrappers.
Each stub logs the receiver/args, the interned name when present, and the guest call site.

**Correction to the previous session's map: `0x2813338` is PostEvent, not SetState.** Its body ends in
`ldr x8,[x23,#0x158]; ldr x0,[x8,#0x60]; ldr x8,[x8,#0xc0]; blr x8` — the same vtable+0xc0 slot as `0x9b9db4` and
`0x281356c`. So the game posts events named `SET_STATE_*` / `PLAY_UI_*` (Wwise event names, not state names) from
121 sites; there is no separate SetState wrapper in this binary.

**What a boot actually does (measured, 5 runs):** a scripted route (language confirm → `kbtype`/`kbenter` name →
A at the prompts) advances the flow (`SET_STATE_FIRST_GAME_START`, `PLAY_UI_COMMON_DECIDE`,
`PLAY_UI_MESSAGE_FUTURE`, `PLAY_UI_COMMON_MENU_OPEN`) and the events reach the wrapper — so the game-side audio
director is alive — but **every engine-side site stays silent for the whole run**: no LoadBank through either API
pair, no gfl bank loader, and the SoundController gate updater `0x8417a8` never executes once.
`PLA_AUDIO_FS_LOG=all` over the same route shows exactly three sound opens (the BGM tables) and **zero .bnk/.pck**.
The engine is therefore never asked to load a bank at any point before the intro; since the resolver falls back to a
direct RomFS open by name (how the field's ENV bank got in), a request would have left an open in the log — the load
is never issued, not mis-pathed.

**The bank files themselves, from the RomFS index (`PLA_build/extracted/v1101/romfs/index.json`):** `Init.bnk`
25,053 B, `Common.bnk` 14,975,844 B, `BGM.bnk` 284,072 B, `UI.bnk` 3,132,665 B are real banks; the
`ENV_BASE_AREA00..06.bnk` are **32-byte placeholders** and `LooseMedia.pck` is 56 B — the streamed media lives in
`Default.pck` (674,249,524 B). `bin/misc/app_config/sound_config.bin` carries the two 32-entry tables:
`bank_names0..13` = Common, UI, NPC, PC, PM_NORMAL, SCENARIO_SCRIPT, BATTLE_SYSTEM, PROPS, BGM, ME, Dialog, ADEMO,
PM_SKILLS, PM_VOICE (rest empty) and `package_names0/1` = Default.pck, LooseMedia.pck. **`Init.bnk` is not in
either table** — the boot's `0x277e5b0` builds its path by hand and only interns it (the struct is destroyed at the
function end), so even the name plumbing for the Init bank never reaches a load call.

**Also measured:** `0x28104e8` (the "engine start" after the config load) is listener/game-object registration
(string "DefaultListener", slots +0x10/+0x18/+0xc0/+0xb8), not bank work. `PLA_AUDIO_SUSPEND=1` (the game's own
boot suspend restored) makes no difference to any of the above — the missing load is not caused by the audboot
patch. The main thread stays in the normal frame-wait cv (`main+0x296d9a4 ← main+0x296b05c ← main+0x2781370 ←
nnMain+0x88`) at 30 fps throughout.

**Open question for the next run (needs a decision):** the previous session saw one bank load in the field
(`ENV_BASE_AREA00.bnk`, 32-byte placeholder) — so the in-game scene path *does* issue loads, and the decisive
experiment is the field with these probes on. Reaching the field in this port currently needs
`TOTK_GPU_ALLOW_SUBMIT=1` (documented as having kernel-panicked macOS 4× on IOGPUFamily), so it is a deliberate,
bounded run. Until then the working hypothesis is: the gfl bank manager (class with vtable `0x40c5b40`, ctor
`0x8dbdb4` ← `0x91c260` ← `0x906d7c` ← `0x906cbc`, load methods `0x8dbe34`/`0x8df3bc`) is constructed and driven by
the in-game scene/audio-director path, and the boot-time Init/UI bank load is part of that same subsystem — the
subsystem just never starts before a scene exists in this port.

### Field run (GPU submission, approved): the bank load exists, but not through the engine facade

With `TOTK_GPU_ALLOW_SUBMIT=1` the scripted route reaches the field (character data loads at frame ~5700, ~190 s)
and the game sets field audio states (`SET_STATE_RIDE_NONE`, `SET_STATE_MIXER_AMBIENT`). Exactly **one** bank is
requested in the whole run: `ENV_BASE_AREA00.bnk`, opened twice by the resolver (localized `English(US)/` probe
then the base name) — the same placeholder load the earlier session saw. `Init.bnk`, `Common.bnk`, `BGM.bnk`,
`UI.bnk` and `Default.pck` are still never touched; the field is silent for the same reason as the menus.

New instruments from this run: a probe on the IO request dispatcher (`0x2d8f2c8`) shows the two requests carry
**type 0** (the bank-open case → `0x2d90504`, where the BKHD version check lives), and a probe on the bank
manager's request enqueue (`0x2e0e3bc`, vtable `0x41abda8` +0x20: appends a 0x50-byte node to the queue at
manager+0x1a0..0x1c4 that the audio thread drains) is installed to name the *producer*. Crucially, **none of the
four AK::SoundEngine API slots fired** (+0x100/+0x108 impls `0x2dcb224`/`0x2dcb9b8`, +0xf0/+0xf8 impls
`0x2dcb558`/`0x2dcb664`), and neither did the gfl bank loaders (`0x8dbe34`/`0x8df3bc`): the one load that does
happen goes through an engine entry the facade probes do not cover. The enqueue probe is the one that will name
it — it needs a field run that gets there, see below.

**Obstacle found: an intermittent freeze after the name entry.** Two of five runs stop presenting at frame 3900
(~130 s) and two more at 4800 (~160 s); the main thread stays in the normal frame-wait cv, the render thread in
`mtl_link_wait_tick`/`h_nvnWindowAcquireTexture`. The run that reached the field did so at frame 5700 with the
same input script, so the freeze is a race, not a route error (the swkbd itself works: `text entry bar shown
(你的名字是？)`). It is independent of the audio work (it also happens with no probes and without GPU submission);
next session should chase it first, because it gates the field measurements.

Mac key codes used by the scripts: `J` (38) = A, `Return` (36) = Plus, key 10 = harmless wait key; `TOTK_INPUT_FILE`
commands `key`, `kbtype`, `kbenter`, `shot`, `threads` all work against this build.

## Audio, round 3 — the render stage is the missing piece (2026-11-02)

**Question.** After the earlier rounds (engine boot suspend fixed, AudioOut adapter fixed, device opens and the
sink plays) the game was still silent with every `[pcmfed]` append all-zero. Round 3 asked *which* stage of the
Wwise pipeline is actually missing, instead of assuming "no banks".

**Method.** Every claim below is a recorded probe/dump, not an inference: an env-gated site-probe family
(`PLA_AUDIO_SITE_PROBE`), fill-path counters (`PLA_AUDIO_FILL_PROBE`), AuBuffer parameter dump
(`PLA_AUDIO_BUF_PROBE`), the full AK API dispatch table dump + trampolines (`PLA_AUDIO_APITAB_PROBE`), a
call site census (`api_calls.txt`, `vcalls.txt`, `bl_xrefs.json`) and the AK dispatch thunk region
(`main+0x2837068..0x2837800`, one thunk per API slot, each a `b <impl>` tail branch).

**What is *not* broken (all measured).** Banks, events, RTPC, device, sink and Core Audio are fine:
host-side `LoadBank` of Init/Common/UI/BGM/ME.bnk returns 1 with real bank IDs; `PostEvent` resolves the event
name (`0x2d77240` name→FNV-1a-32 → `0x2d77020`) and enqueues an action, returning the action's sequence number
(hence "non-zero return" is *not* an AKRESULT error); RTPC volume is 7/7/7 (`SPEAKER_MODE=1`); the sink opens,
starts, appends and stops correctly on the host side. The AK threads (`AK::EventManager`, `AK::BankManager`,
`AK::IOThread`, `AK::OpusDecodingThread`, `AK Suspended`) are all listed by `PLA_THREAD_NAMES=2`.

**What is missing (the root cause).** The engine is never asked to *mix*. Evidence chain:
1. The sink's per-tick render gate `main+0x2df4b14` reads `[device+8]`; it is **always 0**, so every tick takes the
   "prime with silence" branch (`vtable+0x78` → `memset` + `Append`) instead of the mix branch (`vtable+0x70`).
   `[fill] mix` = 0 occurrences, `[fill] silence` = hundreds. That is why every append is zero.
2. The AK mixer→device handoff `main+0x2df49e0` (device `vtable+0x48`, which ends by setting `[device+8] = 1`) is
   **called 0 times**.
3. `PLA_AUDIO_FORCE_RENDER=1` (NOP the gate's `cbz`) makes the mix branch run, but the engine's float mixing
   buffer (`[sink+0x28]`) stays all-zero and no `.wem`/`Default.pck` is ever opened: the engine has no voice to mix.
4. `AK::SoundEngine::RenderAudio(bool)` is the dispatch slot `+0x040` (thunk `main+0x28372bc`:
   `and w0,w1,#1; b 0x2d75fe0`), and in the whole binary it has exactly **one** call site: `main+0x2781474` inside
   the boot sound setup (`main+0x27812dc`), i.e. once before any event is posted. There is no per-frame caller —
   `PLA_AUDIO_APITAB_PROBE` trampolines on the 44 non-empty API slots recorded **zero** calls after boot.
5. Driving `RenderAudio(1)` from the host (hooked at install time on the AK device process `main+0x2e16ef8`,
   `PLA_AUDIO_RENDER_DRIVE=1`, ~44/s, returns 1 every time) keeps the run healthy but still produces no mix:
   the renderer has no voices, because the game objects/actions the events target are registered by the *game's*
   sound manager, which never runs.

**So the defect is upstream of the engine: the game's own per-frame audio update never starts.** That subsystem
is the one that registers game objects, posts the per-frame `RenderAudio`, and loads the real bank set; in this
port it is created but never stepped (see round 2's chain notes, and the round-3 correction: the boot chain
`wrapper → manager ctor → device setup → device start` *does* run once — `sndmgrcreate`, `devsetup`, `chainlast`
each fired once — so "chain never created" was wrong; "per-frame update never runs" is what the data supports).
Also measured and ruled out: `TOTK_ASSUME_FOCUS`, window focus messages, app-state/SoundController gate
`component+0x54` (the sound subsystem's ctor `0x8413a0` never runs, consistent with the same conclusion), and the
`pl:u`/`hwopus` services (both open fine).

**Pitfalls worth keeping.**
- Never put a site probe on the sink init `main+0x2e35b14`: the boot stalls at frame 0 before the first frame.
- Any `memcpy` into `main` text after load faults (SIGBUS, `esr=0x9200004f`) because `runtime/loader.c:258` maps
  text `PROT_READ|PROT_EXEC`. Runtime hooks must either be installed *before* that `mprotect` or go through
  `mach_vm_protect`; this round's first `RenderAudio` hook install happened too late and crashed exactly that way.
- `PLA_AUDIO_APITAB_PROBE` trampoline stride must be 64 bytes (32 overwrote neighbours and stalled boot at
  frame 0). Game C++ vtables are not stored as module VAs in the file (REL/RELA), so scanning `.data` for
  function addresses only finds AK's 24-byte `{fn, BSS slot, 0x403}` patch table.
- A healthy boot/run is `grep -c 'totk-gfx' <log>` ≈ 50–480 and `帧 N` lines ≈ 19+; frame-0 stalls show 8 and 0.

**Fixed in this round (verified by run).** `runtime/audio_sdk.c`'s AuBuffer adapter now follows the SDK's real
`SetAudioOutBufferInfo(buffer, capacity, data_size)` semantics (`sdk.elf` `0x3ac70` stores arg4 in `data_size`,
`+0x20` is never written; `0x3ac80` `GetAudioOutBufferDataPointer` = `ldr x0,[x0,#8]`). It used to treat arg4 as
an offset, so the guest filled `buffer+4096` — past the 4096-byte allocation — and Append read the same
out-of-bounds window. New behaviour: `data_size = arg4`, `data_offset = 0`, DataPointer = `buffer`; a 40 s run
with `PLA_AUDIO_BUF_PROBE=1` shows correct parameters, 1800 appends, no signal, no crash. Also kept from this
round: the `au_core_released_wait` change (block while a started device still has un-released buffers) — without
it the sink is torn down before it ever renders.

**Next session starts here.** Find what *starts* the game's per-frame sound update: the task driver
`main+0x84e4c0` is called ~25–44 times/s through `main+0x29684dc` (`vtable+0x70`), but the object it is handed
looks wrong (`[self]` reads a BSS address). Hook the caller `main+0x29684dc` and print *its* `x0`, resolve that
object's vtable to a class, find the ctor, and see who should set its state to "running". Fixing that should make
`RenderAudio` run per frame on its own, which in turn should open `Init.bnk`/`Default.pck` from the game and let
the mixer produce non-zero samples — the end-to-end check is still `[pcmfed] ... nonzero>0` (or RMS from
`PLA_AUDIO_PCM_DUMP`), never "banks loaded".

## Audio, round 4 — correcting round 3: the render loop *is* driven; no voice is ever created

Round 3 concluded "the game's per-frame audio update never runs" from
`PLA_AUDIO_APITAB_PROBE` trampolining the AK dispatch table (all 44 slots counted zero after boot). That
conclusion is **wrong**, and the reason it was wrong is worth keeping: the game caches the dispatch-table
function pointers in its own objects during boot, long before our table trampolines install, so later calls
bypass them. Hooking the *implementations* instead settles it:

- `PLA_AUDIO_RENDER_TRACE=1` hooks `main+0x2d75fe0` (`AK::SoundEngine::RenderAudio(bool)`, dispatch slot
  `+0x040`): `[rtrace] RenderAudio #N caller main+0x2781478`, >500 entries in 40 s (12–17/s). So the per-frame
  audio update `main+0x27812dc` **is** running and driving `RenderAudio(1)` every frame; `main+0x27812dc` is a
  per-frame update function (it also holds one-shot init under a `ldarb` gate), not boot-only setup.
- `PLA_AUDIO_RENDER_TRACE=2` hooks the inner render body `main+0x2d7e250` (also called once per millisecond by
  the AK EventManager thread, `main+0x2e33118`). At entry: `qcount=0 suspended=0 dev_list=<non-null>
  dev_count=1` — engine live, not suspended, exactly one output device, no pending commands.
- `PLA_AUDIO_CMD_PROBE=1` hooks the command pipeline: `main+0x2ddcc24` (job enqueue for a posted event, called
  with the job manager at `[0x430a170]`), `main+0x2d7de5c` (AK command-pool allocator; a posted event allocates
  a 0x34-byte action at `main+0x2d770d4`), `main+0x2dc98a8` (the render body's "no pending command" path) and
  `main+0x2d7ead4` (message-ring dispatch; ring object `[0x430a128]`, read `+0`, write `+8`, capacity `+0x20`).
  Measured over 30 s: 5 event jobs enqueued, ring advances by one 20-byte message per event, render body keeps
  taking the no-command path — i.e. events do reach the engine's message ring.

What is still missing is the voice. With 5 banks force-loaded (`Init/Common/UI/BGM/ME` all `-> 1`), events
resolving in the engine's event table, and a hand-posted `PLAY_UI_COMMON_DECIDE` accepted
(`[testev] PostEvent(...) -> 1`), **no voice is ever created**: the mixer→device handoff `main+0x2df49e0`
(device vtable `+0x48`, the function that ends with `mov w8,#1; strb w8,[x19,#8]`) is called **0 times**, the
sink render gate `[device+8]` stays 0 so every tick takes the silence path, and the engine's float mixing buffer
(`[sink+0x28]`) stays all-zero. `[pcmfed] ... nonzero=0/N` follows from that. `LoadBank("Default.pck")` returns
2 (rejected) — the `.pck` container cannot go through `LoadBank` directly.

**So the current state is: banks load, events resolve and reach the message ring, the mixer is driven every
frame, the device sink and Core Audio path are healthy — but the per-event work that would instantiate a voice
never happens.** The next step is to follow one message from the ring (`main+0x2d7ead4`, message type is a
`u16` at `+2`) into its handler and find where voice creation is skipped or refused (bus/output-device
mismatch, unregistered game object, or a missing `RegisterGameObj`/listener in the engine's own tables).

**Diagnostics added this round (all env-gated, default off).** `PLA_AUDIO_RENDER_TRACE=1|2`,
`PLA_AUDIO_CMD_PROBE`, `PLA_TEST_EVENT`/`PLA_TEST_GO`, `PLA_AUDIO_CALL_API`. Install them from
`runtime_audio_bank_bootstrap_install()` (loader time, text still writable) — installing at run time faults
(`esr=0x9200004f`). Site stubs must save **every** argument register they clobber (`x0`–`x7`); saving only
`x0`/`x1` around the hook call silently corrupted the pool allocator and stalled boot at frame 0.

## Audio, round 5 — the event pipeline is fully alive; the gap is voice instantiation

Following round 4's correction, this round traced one posted event end to end through the engine's message
ring. The ring is object `[0x430a128]` (read `+0`, write `+8`, capacity `+0x20`), drained by
`main+0x2d7ead4` (called from the render body `main+0x2d7e250`, which the AK EventManager thread runs every
1 ms from `main+0x2e33118`). Measured with `PLA_AUDIO_CMD_PROBE` + `PLA_AUDIO_CMD_SITES=4`:

- `PostEvent` → job manager `[0x430a170]` → `main+0x2ddcc24` allocates a 0x34-byte action
  (`main+0x2d770d4`) and writes a 20-byte message into the ring. Messages accumulate (pend 20 → 96 → 148)
  and are then **drained to zero** (rd advances in 0x14 steps) — the consumer works.
- Message types observed: `0x36` first (handler `main+0x2d7f448`), then `0x2` (`main+0x2d7ec5c`, the
  game-object action path) and `0x1` (`main+0x2d7ed80`). Dispatch is a jump table at `0x399ed90`
  (`main+0x2d7ebfc + 4*offset`, 0x3b entries); both `0x2` and `0x1` handlers look a game object up in
  `[0x430a140]` (bucket count `+0x38`, buckets `+0x30`, node `{next@0, id@8, obj@0x10}`) and then call the
  action executor `main+0x2de0d64` with `(eventmgr=[0x430a160], gameObject, &params, &aux, 1, 0, s0)`.
- Loading **all 15 game banks** (`Init, Common, UI, BGM, ME, PC, NPC, PM_NORMAL, PROPS, SCENARIO_SCRIPT,
  BATTLE_SYSTEM, Dialog, ADEMO, PM_SKILLS, PM_VOICE` — every `LoadBank -> 1`, 40 s, healthy run gfx=445)
  changes nothing: PCM stays all-zero and the device handoff `main+0x2df49e0` (device vtable `+0x48`) is
  still never entered. So "missing bank" is ruled out for the whole set, not just the core five.
- `main+0x2de0d64` gates on `[x0+8]` (`ldr w8,[x0,#8]; cbz w8, 0x2de0dd0`) where `x0 = [0x430a160]`; at
  the time our probe samples, both `[0x430a140]` and `[0x430a160]` read 0, so the object registration /
  event-manager initialisation state of the engine is the prime suspect for why the play action never turns
  into a voice.

**Remaining gap in one line:** banks load, `PostEvent` succeeds, the message ring drains, and the action
executor is reached — but no voice object is ever allocated (the AK command pool only ever allocates 4–52
byte payloads) and the mixer→device handoff never fires. Next step is to hook `main+0x2de0d64` (safe function
entry, prologue `d10183ff fd000be8 f9000ff9`) and follow its `[x0+8]` gate: if it is 0, find which
initialisation should set it and why the game's boot path does not.

**Probe pitfalls added this round.**
- A hook on `main+0x2d7ebf8` / `main+0x2d7ec5c` / `main+0x2d7ecb0` (mid-function dispatch labels) stalls boot at
  frame 0 even though the three prologue words are replayed; use `PLA_AUDIO_CMD_SITES=4` (or fewer) to stay safe.
- `PLA_TEST_EVENT` (PostEvent from the bank-bootstrap hook at boot time) also stalls boot at frame 0 — post
  events only after the engine is fully up.
- Guest globals must be read through the module base (`g_site_modbase + 0x430a140`); using the bare constant
  faults and silently wedges the run (looks exactly like "boot stalls at frame 0").
- Hook stubs must preserve every argument register they clobber (x0–x7); saving only x0/x1 corrupted the AK
  command-pool allocator (`main+0x2d7de5c`) and wedged boot.

## Audio, round 6 — the game's own sound system is never constructed

Round 5 left one question: the engine accepts events and drains its message ring, yet never creates a voice.
This round answered it by probing the game side instead of the engine.

- The engine's **game-object table is empty**: `[0x430a140]` points at a valid table object
  (`0x1004555baa0`) whose bucket count (`+0x38`) stays 0 for the whole run, so it holds zero game objects.
  Wwise can only start voices for a registered game object, so every action the game posts has no target.
- The game never calls `RegisterGameObj` at all: hooks on the `+0x050` / `+0x058` API thunks
  (`main+0x2837330` / `main+0x2837394`, installed after their argument-range branch) record **zero calls**
  in a 30 s boot run.
- The reason is upstream: the game's sound manager is never constructed. The only path that builds it is
  `main+0x1dd82b8` (scene sound init) → `main+0x854984` → `main+0x85be0c` (factory) → `main+0x83fc18`
  (manager ctor). Hooks on the factory (`main+0x85be0c`, epilogue `f81d0ff5 a9014ff4 a9027bfd`) and on the
  scene init (`main+0x1dd82b8`, `d100c3ff f9000bf3 a9027bfd`) both report **zero calls** over a 30 s boot run
  *and* over a 210 s run that reaches frame ~3626 through the name-entry prompt (open intro). Only the engine
  itself is up: init, device, sink, render loop, `PostEvent`, `SetRTPC` (volumes 7/7/7).
- Static shape of the chain: `main+0x85be0c` reads an init flag (`ldarb` at `0x4279000+0x520`, bit 0), takes a
  lock, allocates `0x118` bytes from a pool, calls `main+0x83fc18`, and stores `{self, ptr}` at object `+8`.
  Its only caller is `main+0x854a68` inside `main+0x854984` (9136 bytes — the same function that is the only
  game-side `UnloadBank` caller). `main+0x854984`'s only caller is `main+0x1dd8304` inside `main+0x1dd82b8`,
  and `main+0x1dd82b8`'s only caller is `main+0x1dd7dc4` inside `main+0x1dd7c68`, which has **no BL callers**
  at all: it is reached through the BSS slot `0x4158a68`, one of the `{fn, slot, 0x403}` initialiser records
  that fill this NSO's BSS-resident function-pointer table (record at `0x34e0070`). So the whole sound system
  hangs off an indirect entry that our port never drives.
- Practical consequence for the port: this is not a bank problem and not an engine problem. Until the game's
  scene sound init runs, PLA cannot register a game object and therefore cannot make a single voice; the
  engine keeps rendering silence into the sink, which is exactly what `[pcmfed] nonzero=0/N` reports.

**Where to go next.** Find what fills/invokes slot `0x4158a68` (that record table is at `0x34e0040`, step 24,
each `{fn, slot_addr, 0x403}`), and whether the trigger is a scene-load step our scripted run never reaches
(the run only reaches the opening after the name prompt). If the trigger is a scene transition, driving the
game into actual gameplay should make `main+0x85be0c` fire and voices appear; if it never fires even in
gameplay, the port is missing whatever task/scene driver is supposed to call it.

**Probe pitfalls added this round.** `g_site_modbase` is only set by the probes that compute it themselves —
`PLA_AUDIO_CMD_PROBE` now sets it too, otherwise every guest-global read silently returns 0 (read `0x10ed04000
+ 0x41ab268` to confirm: N must be 1024). Function entries beginning with `cmn x1,#0x21 / b.hi` (the AK API
thunks) cannot be hooked at offset 0 with the replay-three-words stub; hook after the range-check branch.

## Audio, round 7 — the command-counter bug, and where the missing sound object is born

**A probe bug invalidated part of round 6.** `au_cmd_note()` opened with `if (idx >= 8) return;` while the
counter array was `static int g_cmd_n[8]` and the interesting sites use indices 10..13 (reggo50, reggo58,
sndfactory, sndsceneinit). Every "0 calls" reading for those four sites was therefore an artefact of the guard,
not evidence. Fixed: array is now `g_cmd_n[32]` and the guard is `idx >= 32`. Re-checked with the fixed guard:
`sndfactory` (main+0x85be0c) and `sndsceneinit` (main+0x1dd82b8) still never fire, and neither does the
registration thunk path at main+0x2837330 — so the round-6 conclusion stands, but it now stands on a working
counter.

**The missing object is a class instance, and its construction chain is fully static now.** Slot 0x4158a68 is
not a stray function pointer: 0x4158a18 is a class vtable (a constructor at main+0x1dd7640 stores
`str x8,[x0]` with `x8 = base+0x4158a18`, and also seeds `[x0+0x48] = base+0x4158a88`, `[x0+0x50] =
base+0x4158ad0`). The scene-sound entry main+0x1dd7c68 is that class's virtual method at vtable offset +0x50.
Construction chain (each step is the only caller of the next):

    main+0x1844770 @ +0x2adc        big scene/level update function, no BL callers (invoked as a virtual)
      -> main+0x1dd7410
        -> main+0x1dd755c
          -> main+0x1dd971c
            -> main+0x1dd9874       factory: checks flag [[base+0x4279000]+0x520] bit0, allocates 0x108 bytes
                                    through [x19]vtable+0x10, then calls the constructor
              -> main+0x1dd7640     constructor (stores vtable base+0x4158a18, {owner,self} at +8)
                -> ... main+0x1dd7c68 virtual +0x50  (lazy guard on [this+0x88])
                  -> main+0x1dd82b8 -> main+0x854984 -> main+0x85be0c -> main+0x83fc18

So the whole game-side sound object graph hangs off one virtual method of the scene update function
main+0x1844770. Nothing in that chain runs in our port — that is the single missing link for PLA audio.

**Host-side registration experiment (negative, but it narrows the API).** The five AK "deferred command"
thunks all load the engine ring themselves from `[0x430a128]` and enqueue `{command type in w1, size in w2,
game-object id in x1}`: main+0x28372c4 and main+0x2837328 enqueue type 0xb / 0x14 bytes, main+0x283738c
enqueues type 0xc / 0xc bytes, and main+0x28373ec (float-validated, 3D matrix arguments) does the same after
computing a distance. Calling main+0x2837328(go=0,1,2) from the host at bank-bootstrap time returns 1 three
times (the command is accepted) but the engine's game-object table (base+0x430a140, bucket count at +0x38)
stays at 0 buckets — so command type 0xb is not "register game object", and self-registering game objects
from the host is not enough to make voices appear.

**Left for the next round.** Give the scene update function main+0x1844770 a way to reach the
main+0x1dd7410 call (probe at its +0x2adc offset without hooking mid-function — e.g. count the function entry
and then the 0x1dd7410 call site separately), or construct the sound object directly from the host by calling
the factory main+0x1dd9874 with the right owner object (it needs an x0 whose vtable provides `+0x10` as an
allocator and a matching `+0x8` init flag).

## Audio, round 8 — the game's sound subsystem is never constructed at all (2026-11-02 23:15)

Round 7 narrowed the silence to "the game's sound object is never built". This round drove that to a
reproducible, quantitative conclusion by hooking the whole static chain and dumping the fixup blob at runtime.

**Runtime result (240 s real gameplay run, 2367 frames, gfx 2458 lines, healthy, zero FAIL).** Every site below
was hooked successfully ("hooked", prologue matched) and every one of them fired **0 times**:

| site | module offset | role |
|---|---|---|
| sceneupd | main+0x1844770 | scene sound update (the only caller of the chain) |
| ch1/ch2/ch3 | main+0x1dd7410 / 0x1dd755c / 0x1dd971c | scene-sound creation chain |
| factory | main+0x1dd9874 | builds the 0x108-byte sound object |
| scinit | main+0x1dd7c68 | vtable+0x50 scene init → 0x1dd82b8 → 0x854984 |
| famctor/famsib/famupd | main+0x1844514 / 0x1843ebc / 0x1844700 | the whole class family (base + sibling ctors) |
| sndtop/sndbig | main+0x1877a4c / 0x18b5b5c | top-level sound/field system + its 24 KB aggregate init |
| ready | main+0x8470dc | writes app_state = 6 |
| appst1/appst2/appst3 | main+0x2844350 / 0x2848360 / 0x1b01684 | write app_state = 1 or 4 (the values sound needs) |

So the game's *application/scene framework* — the only code that constructs the sound subsystem — does not run
in the port, not even during a full field session. That is not a missing patch in one function; it is a whole
subsystem that never starts.

**Blob dump (`PLA_DUMP_VTAB=1`, host-thread read of main+0x4130000, 0x4000 bytes).** The `{fn, slot, 0x403}`
fixup records point at a pointer table at main+0x4130000 (392 records seen: sceneupd→+0x108, ctor→+0x0a0,
sndtop→+0x2770, …). At runtime **only one of the 2048 8-byte slots is non-zero** (+0x0 = main+0x7a7b80, a
libc++ helper installed by a RELA, not by the record table). Everything else — +0x98 (the family's primary
vtable), +0x108 (sceneupd), +0x2770 (sndtop) — reads back **zero**.

That closes the loop: the pointer table is empty, so no virtual dispatch into that family can happen either;
combined with the zero-hit hooks it means "the framework never initialises the sound classes", both from the
call side and from the table side.

**Real defects fixed and verified this session (kept, env-free):**
1. `runtime/audio_sdk.c` — `au_set_buffer_info()` stored the 4th argument as `data_offset` and forced
   `data_size = 0`, and `au_buffer_pointer()` returned `buffer + data_offset`. sdk.elf 0x3ac70 proves the 4th
   argument is `data_size` (`str x3,[x0,#0x18]`; `+0x20` is never written) and 0x3ac80 is a plain
   `ldr x0,[x0,#8]`. The old code made the guest render 4096 bytes past a 4096-byte allocation on every append.
   Fixed to `data_size = arg4; data_offset = 0;` and `return buffer;`.
2. `runtime/audio_out.c` — `au_core_released_wait()` only blocked while "priming"; the game polls for a released
   buffer right after consuming one and, getting "none", tore the output sink down before a single buffer had
   rendered (≈10 sink rebuilds/s, mix path never entered). Now it waits whenever the device is STARTED and the
   queue still holds unreleased buffers. With this fix the engine's mix path is reached for the first time
   (`[fill] mix` fires at ~30 Hz under `PLA_AUDIO_FORCE_RENDER=1`), and the sink is stable (11 opens per 260 s).

**Left for the next round.** Decide between (a) continuing down C — find why the port's boot never reaches the
game's application/scene framework init (check `load process`/`FsSubThread` init order and whether the port's
scene entry bypasses it), or (b) going back to B — have the host run the sound chain once on a guest thread
(`main+0x1dd7410` with the two real arguments: `[base+0x4279680]→+0xc8` and `[base+0x427b160]→+0x388`), which
is the shortest path to real audio but bypasses the framework.

## Audio, round 9 (2026-11-03): playback path traced end to end, AK mixer still silent

**Correction to round 8.** Round 8 claimed "the pointer table at `main+0x4130000` is empty (only 1 non-zero
slot)". That was a parsing bug in my own log filter: the printf format is `[vtblob] +0x00000 = main+0x…` while
the regex expected `+0` followed by hex digits without the `0x` prefix. Re-parsing `/tmp/pla_re/vtab2.log`
correctly gives **1787 non-zero slots** across the four pages `0x4130000..0x4133fff` — the table is full, and
the `{fn, slot, 0x403}` fixup records are applied; what differs from the records is a systematic one-record
shift, which is still unexplained but is not "empty".

**What this round established with runtime probes (all env-gated, off by default):**

- `main+0x2de0d64` is the per-event instance limiter and `main+0x2de0fe0` the action-list executor; both are
  entered normally for every posted event (`[act]`, `[exec]`).
- The action lookup (`main+0x2dddc48`), the play action (`main+0x2de13f4`), its start call (`main+0x2e0ed58`)
  and the parameter-preparation call (`main+0x2e0f0a8`) all execute when a real UI/dialogue event fires
  (`[actlookup]`, `[actplay]`, `[post1]`, `[post2]`). So the event → action → play chain is alive.
- `main+0x2e0f0a8` is *not* voice creation: it only computes playback parameters (voice-count division by
  `[base+0x41ab25c]` = 21 max voices per device, start/stop windows) and stores them.
- With `PLA_AUDIO_FORCE_RENDER=1` the device mix path runs (`[fill] mix` fires) but the engine's float mix
  buffer `[sink+0x28]` is **all zeros every sample**, so the sink keeps producing silence. The mixer itself
  never writes into that buffer, therefore `[pcmfed]` stays `nonzero=0/N`.
- AK engine state read from a host thread at t=30 s: command queue `[engine+0x240/0x248] = 0`, audio-plugin
  render list `[engine+0x250/0x258] = 0`, device `[dev+0x28] = 0` (no float mix buffer attached on the device
  object), `[dev+0x148] = 0` (no pending PCM), `[dev+0x8] = 0` (render gate clear). `maxvoices = 21`.
- No `.wem` and no `Default.pck` open has ever been observed, i.e. no streaming source was ever started.

**Tooling lessons (cost real time, worth recording).**
1. A probe stub must preserve *all* caller-saved registers, not just `x0..x3`: `x4..x7` are clobbered by the C
   logging call and corrupted the guest, deadlocking it (`CPU 0%`, guest stuck in `flockfile`/`runtime_gate`).
   The stubs now save/restore `x0..x7` and `q0..q3`.
2. Never hook a **mid-function** address: at a function entry only the argument registers are live, but
   mid-function the live set includes `x8..x15`; replaying three prologue words with a stub that only restores
   `x0..x7` corrupts the frame and hangs the game after the intro movie. Mid-function sites were removed.
3. Guest hot paths must use the lock-free `au_log_line()` (`vsnprintf` into a stack buffer + `write(2)`);
   `fprintf(stderr)` deadlocks under `flockfile` when the process is asked for a thread dump.

**Next.** Find the function that should render the mixer output into `[sink+0x28]` and call the device slot
`vtable+0x48` (`main+0x2df49e0`, zero hits today). The platform thread `main+0x2e16ef8` only pulls from
`[dev+0x148]`, and that pointer is set by the handoff function, so the missing link is the AK mixer render pass,
not the host AudioOut adapter.

## Audio, round 10 (2026-11-03): the AK event-dispatch path drops on an empty game-object table

Static reading of the engine's event dispatch (`main+0x2d7e250` → message ring → per-type handler):

- message type `0x1` (handler `main+0x2d7ed80`) is the "play event on a game object" path. It looks up
  `[engine+0x140]` (the AK game-object hash map), and **both** the "no buckets" and the "id not found" cases
  jump straight to `msgdone` — the message is dropped silently. The same lookup exists in the type `0x2`
  handler; only `gameObjectID == -1` takes the global path at `0x2d7ed1c`.
- the successful path calls `main+0x2d81050` (execute actions for that object) → `main+0x2d813f0` (action
  executor) → `main+0x2df013c` / `main+0x2df0870` (the ~1.8 KB play-action pair, the place a voice would be
  created). Four new env-gated hooks now count these (`[akact]`), so a single user-visible event is enough to
  tell whether AK ever *attempts* playback: reaching `playbeg` with no voice ⇒ the failure is inside the play
  action; never reaching `exec` ⇒ the message really was dropped.
- engine state at t=30 s still shows `[engine+0x240/0x248] = 0` (command queue empty) and
  `[engine+0x250/0x258] = 0` (audio-plugin render list empty), and the game-object map has 0 buckets, so
  today every game-object-targeted action is dropped by the lookups above.

**Two failed injection routes (both deadlock, recorded so nobody repeats them).**
1. Posting `PLAY_UI_COMMON_DECIDE` from a host thread: the very first call blocks (the engine takes its own
   mutexes).
2. Posting it from the `RenderAudio` hook (`main+0x2d75fe0`) — the engine is already inside its own lock there;
   the run stalls at the frame where the injection fires and never prints the tag.
A third route — hooking the per-frame audio update (`main+0x27812dc`) to inject — also stalls boot: that
function runs several times per frame, so the "wait N frames" guard is reached almost immediately.

**Practical consequence.** The remaining question ("does AK ever try to play?") is answered fastest by one
short real play session with `PLA_AUDIO_SNDCHAIN_PROBE=1`: the `[akact]` counters then show how far the
dispatch gets for a genuine menu/dialogue sound.

## Audio, round 11 — the game posts events to game object 1 while the engine's game-object table is empty

Measured with `PLA_AUDIO_SNDCHAIN_PROBE=1` (both a healthy 65 s boot and a 150 s run that reached the field):

    [sndchain] postevent #1 eventID=0x491de2d2 go=0x1 gobuckets=0 caller main+0x2d7733c
    [sndchain] postevent #2 eventID=0x368e8584 go=0x1 gobuckets=0 caller main+0x2d7733c

* `go=0x1` is the game-object id the game passes to `AK::SoundEngine::PostEvent`; `gobuckets=0` is the bucket count
  of the engine's game-object table. The table object is the one the message dispatcher passes as `x0` to
  `main+0x2df2694` / `main+0x2d7ec5c`: `[base+0x430a140]`, buckets at `+0x38`, bucket array at `+0x30`, nodes
  `{next@0, id@8, obj@0x10}`. **It is not `engine+0x140`** — reading that slot yields the poison value
  `0xb0000000b`, which is what made an earlier round-11 measurement look like a range error.
* Static confirmation of the drop: `main+0x2d7ec5c` does `ldur x8,[x26,#4]` (target id) -> `cmn x8,#1; b.eq 0x2d7ed1c`
  (only id == -1 takes the global path) -> `ldr x9,[x25,#0x140]` -> `ldr w10,[x9,#0x38]; cbz w10,0x2d7ebf8`
  (zero buckets = drop, with no error and no voice). `main+0x2d7ed80` (type 1) has the identical guard.
* Why this is the whole bug: no game object is ever registered, so every event the game sends is discarded before
  an action can be resolved, no voice is created, the AK mixer never hands data to the device
  (`main+0x2df49e0` stays at 0 calls), and the sink therefore fills silence forever
  (`[fill] mix` never runs unless forced; `[pcmfed] nonzero=0/N`).
* Evidence that the registration path really is never used: the command-pool histogram of a live session contains
  only 4/20/24/52-byte commands (RenderAudio request, type-0x36 producer, SetRTPC, PostEvent action). The
  register-family implementations (`0x2d760e0`, i.e. API slots +0x048/+0x050/+0x058) would allocate 28/36-byte
  commands, and none appear. Hooks on the post-validation entries `main+0x2837330` / `main+0x2837394` count zero
  calls as well.
* Next probe (not yet answered): find who is supposed to call the register API. The game-side sound system builds
  through `0x1dd7c68 -> 0x1dd82b8 -> 0x854984 -> 0x85be0c -> 0x83fc18` (scene update `0x1844770` and its steps
  `0x1dd7410/0x1dd755c/0x1dd971c/0x1dd9874` were measured at zero calls, while the sibling family
  `0x1843ebc` — reached from `0x1877a4c -> 0x18b5b5c -> 0x18b6b4c` — does run). The registration call should be
  inside one of those, most likely right after the manager is constructed.

Diagnostics added this round (all env-gated and off by default): `postevent` site on `main+0x2d77020` printing
`eventID/go/gobuckets`, plus `usercmd` on `main+0x2d76f2c` (the PostEvent-by-id entry, never called), and hardened
pointer reads (`AU_PTR_OK`) so a probe can never fault the guest when the engine object does not exist yet.
* Confirmed the other half: the game never calls the register family either. A site on the post-validation entry of
  API slot +0x048 (`main+0x28372cc`, the thunk that validates `id < 0x21` and enqueues a 20-byte type-0xb command)
  reports **zero calls** across a 70 s interactive run, together with zero calls on `main+0x2837330` (slot +0x050)
  and `main+0x2837394` (slot +0x058). So on this build the game posts events for game object 1 while never
  registering any game object: the register call site is in game code that never executes here.

## Audio, round 12 — the drop is confirmed as the only blocker, and the two obvious workarounds both fail

Two experiments, both committed behind env vars and off by default:

1. `PLA_REG_GO_RT=<frame>[,<id>...]` enqueues the register-family command (`main+0x2837328`, AK API slot +0x050)
   from inside a RenderAudio tick, i.e. on a guest thread with the engine already running, and re-reads the table
   a few thousand ticks later:

       [reggort] thr=60 slot+0x050(go=1) -> 1 buckets_before=0
       [reggort] thr=60 slot+0x050(go=2) -> 1 buckets_before=0
       [reggort] buckets_after=0 tbl=0x1004555baa0

   The command is accepted (returns 1) but the table never gains a bucket, so type 0xb is not the
   register-game-object command — or it is and the deferred command is never consumed (the render loop's command
   count `[0x430a248]` is 0 on every entry, so this cannot be distinguished from "consumed and ignored" yet).

2. `PLA_FORCE_GLOBAL_GO=1` patches the game-object argument of the shared PostEvent wrapper (`main+0x28313478`,
   `ldr x1,[x22,#8]`) to `AK_INVALID_GAME_OBJECT` (-1) so events take the global branch at `main+0x2d7ed1c`
   instead of being dropped by the empty table. The patch applies cleanly and the guest then crashes with
   `signal=0xb`, `fault=0xffffffffffffffff`, `esr=0x92000004` — something dereferences the id directly, so -1 is
   not a usable substitute for "registered". Reverted to off-by-default.

Next: the hash-map insert into that table (writer of `[table+0x38]` / `[table+0x30]`) is the function that
actually registers a game object; find it, then its callers — that is exactly the game-side code path that never
runs here, and the one place a fix can go.

## Audio, round 13 — surgical fallback for the empty game-object table

`PLA_GO_GLOBAL_FALLBACK=1` rewrites the single instruction that drops events for an unknown game object
(`main+0x2d7ec7c`, `cbz w10, <message done>`) into `b #+0xa0`, i.e. the path a global event (`id == -1`) takes at
`main+0x2d7ed1c`. Unlike forcing the id to -1 at the wrapper (which crashed because something dereferences the id),
this keeps the real id in the message: only the table lookup decision changes, and the action is then resolved and
executed without consulting the table. With the patch the game runs normally (`signal=0`, frames 3300 in a 200 s
save-load run) but scripted runs cannot validate audio, because in automation the action executor
`main+0x2de0d64` is never reached at all (`action=0` while `postevent=12`) whereas in a real play session it fires
(measured 40 calls earlier). Manual play is therefore required to judge whether the fallback produces sound.

## Audio, round 14 — the play-request queue is drained, yet no voice is ever born

`PLA_DUMP_PLAYQ=1` samples the play-request holder `[main+0x430a150]` from the render hook (guest thread, every 16
render ticks). During a real play session the holder's vector allocated (`vec=0x1005884eaf0`, `cap=5`) and `max`
reached **3**, but `cnt` was 0 at every sample: requests are queued and drained within a second, so a consumer does
exist. The device still only fills silence (`[fill] mix` 0 times, `[pcmfed] nonzero=0/15600`), so the consumer does
not turn requests into voices.

`PLA_DUMP_VT=0x41aaa60` dumps the runtime vtable of the play-request object: `+0x00`=main+0x2dde98c (free),
`+0x18`=0x2de3ebc, `+0x20`=0x2de3ecc, `+0x38`=0x2de3ebc, `+0x40`=0x2de401c, `+0x58`=0x2de5e78, `+0x60`=0x2de5e88,
`+0x78`=0x2de5e78, `+0x80`=0x2de5fd8, `+0x98`=0x2de6648, `+0xa0`=0x2de64f8, `+0xb8`=0x2de6648. Each real method looks
up an engine TLS singleton (`tpidr_el0` + 0x200020 slot, fallback `[main+0x42f1798]`) and forwards to
`[singleton->vtable + 0xc0]`, i.e. the request interfaces are thin forwards into the engine, which is where voice
creation has to happen. Those slots are the next thing to instrument.

## Audio, round 15 — the queue is fine, the request interface is not used, and the "misaligned relocations" lead is dead

The play request is created, queued and drained every second, yet no voice appears. This round pinned down
each hop and then disproved the one lead that looked like a systemic port bug.

1. `PLA_DUMP_PLAYQ=1` (guest thread, sampled inside the RenderAudio hook every 16 calls) reads the request
   vector at `[base+0x430a150]` (base@+0, count@+8, capacity@+0xc). In a real 10200-frame play session the
   queue reached `max=5`, `cnt=0` and `cap=5` — so requests *are* enqueued and *are* drained within a second.
2. `PLA_DUMP_VT=<hex>` dumps 24 slots at any address. The request object's vtable (`main+0x41aaa60`) holds
   five real methods (`0x2de3ecc`, `0x2de401c`, `0x2de5e88`, `0x2de5fd8`, `0x2de64f8`, all with prologue
   `{0xf81d0ff5, 0xa9014ff4, 0xa9027bfd}`) that fetch the TLS engine singleton, do `ldr x8,[x8,#0xc0]` and
   tail into it. `PLA_REQ_PROBE=1` hooks all five. In a real manual session they were called **zero times**:
   the queue is drained through a different path than the request's own interface.
3. The queue helpers around `[base+0x430a150]` were identified: `0x2e0ef5c` (walks the holder+0x10 vector and
   moves each request's state `[req+0x3c]` 3->4 / 2->1), `0x2e0efb8` (drops an entry from `[req+0x28]`),
   `0x2e0f058`/`0x2e0f080` (state moves 1->2, 4->3), and `0x2e0f0a8` (the old `post2` probe, 880 bytes) which
   contains a single `bl 0x32a3040` and **no call that could create a voice**. So draining a request at this
   layer does not allocate a voice either.
4. **Correction — there is no relocation misalignment.** A `PLA_DUMP_SLOT=1` check compared five BSS slots
   against the 24-byte `0x403` record tables in `.rodata` and reported five mismatches, which looked like a
   systemic off-by-one in whatever applies those records. It was a misreading: the tables are plain RELA
   entries `{r_offset, r_info=0x403, r_addend}`, and `loader.c:223` applies them correctly. Anchoring the
   triples on `r_info` instead of on `r_offset` makes every slot match its record exactly
   (`0x4176018 <- 0x2837788`, `0x4158a68 <- 0x1dd83ac`, `0x4130108 <- 0x1848d74`, `0x4130100 <- 0x1844770`,
   `0x4130110 <- 0x1848fb0`). The probe now encodes the record's own `r_addend` as the expected value and
   prints no mismatch, so this lead is closed.
5. Unrelated GPU crash seen twice during long manual sessions: `[nvn-mtl] FATAL 内存池 … 建零拷贝缓冲失败`
   (`mtl_pool_add` -> `h_nvnMemoryPoolInitialize`), once at frame 600 and once near frame 4500 with 1.0-1.1 GB
   resident. A clean build survived a 2400-frame session without it. This is a host GPU memory-pool issue,
   independent of audio, and worth its own investigation.

Where this leaves the audio bug: banks load, events resolve, actions execute, play requests are created and
drained — but no voice is ever allocated and the AK mixer never hands a buffer to the device, so the sink keeps
filling silence. The next probes should sit on the voice pool itself rather than on the request plumbing.

## Audio, round 16 — direction 1 answered: the play-request subsystem is bookkeeping only

Chased "does anything ever allocate a voice" through the whole request pipeline. Answer: no, and the reason is
structural — the only path that could hand a request to the engine is never taken.

1. The per-frame consumer of the request vector is in the AK render body (`0x2d7e250`):

   ```
   0x2d7e3ec ldr x25,[x8,#0x150]      ; holder  (vector A at +0, vector B at +0x10)
   0x2d7e3f4 mov x2,x25
   0x2d7e3f8 bl  0x2e0f418            ; process(holder, frame, holder)
   0x2d7e3fc add x2,x25,#0x10
   0x2d7e404 bl  0x2e0f418            ; process(holder, frame, holder+0x10)
   ```

   so the queue really is drained every frame (matching the measured `max=5 -> cnt=0`).
2. `0x2e0f418` walks the vector and switches on the request's state `[req+0x3c]`: state 1 calls
   `0x2e0e5cc(req, frame)`; when that returns with bit0 set the request is *deleted* (the block at
   `0x2e0f52c` ends in `blr` through `[base+0x432d150]`, a free callback). States 4 and 2 do frame-window
   bookkeeping (`[req+0x18]`/`[req+0x24]`) before moving to state 1.
3. `0x2e0e5cc -> 0x2e0e740` contain **no calls at all** (pure arithmetic), and `0x2e0f418` has exactly one
   `bl`. There is no allocation, no vtable dispatch into the sound engine, nothing that could create a voice.
4. The only interface that could reach the engine is the request object's own five methods (they fetch the TLS
   engine singleton and tail-call `[engine]+0xc0`), and `PLA_REQ_PROBE` measured **zero calls** for all five in a
   real manual session. So the request never reaches the engine dispatch layer.
5. Combined with earlier rounds: the AK mixer pass `0x2df49e0` (device vtable `+0x48`, which sets the device's
   render flag) is never invoked, the render body's plugin array `[0x430a250]` has count 0, and the device only
   ever runs the memset path. So the chain is inert at the mixer, and the request plumbing faithfully moves and
   deletes requests without ever asking for a voice.

Conclusion for direction 1: chasing voice-pool allocations inside the game-side action path is a dead end — that
path never reaches the voice manager. The blocker sits one level below, on the AK side: something must drive the
mixer pass (`0x2df49e0`) / register the output plugin, and that piece is missing or inert in this port.

Suggested next direction: find who is supposed to invoke `[dev+0x138]->vtable[0x48]` (or fill `[dev+0x148]` with
pending PCM so the platform thread takes the mixing branch), and check the two gates `[0x430a1cd]` and
`[0x430a248]` in `RenderAudio`.

## Audio, round 17 — the two render gates, and the sink the mixer never touches

Runtime dump of the AK engine block (`PLA_DUMP_ENG`, extended this round) at t=30 s in a scripted boot:

- `[0x430a1cd] = 1` — nonzero, i.e. `RenderAudio` takes the "signal an external render thread" branch
  (`0x2d7605c: ldrb w8,[0x430a1cd]; cbnz -> 0x2d7606c`). It still reaches `0x2d7e250` when its bool argument
  has bit0 set (the game passes 1), so rendering is not skipped, but the engine believes another thread owns it.
- `[0x430a240] = [0x430a248] = [0x430a250] = [0x430a258] = 0` — the command queue is empty and the plugin
  array is empty, i.e. no audio plugin (mixer/effect) has been registered with the engine.
- `[0x430a1a0] = 0x40000000000ff`, `[0x430a1c8] = 0x101000003e8` (1000 = the event-manager period in ms/us).
- The device `dev[0]` exists (`[0x430a7e0]`), `dev_count = 1`, `dev+0x138` points at the sink object
  `0x10044fcdc00`, and both `dev+0x148` (pending PCM) and `dev+0x150` are 0.
- The sink's full vtable was dumped for the first time:
  `+0x00 0x2e3599c, +0x08 0x2e359bc, +0x10 0x2e35df0, +0x18 0x2e360ec, +0x20 0x2df49c4,
   +0x28/0x30 0x2cfae24/0x2cfae2c (return 0), +0x38 0x2e35b14 (init), +0x40 0x2e36078,
   +0x48 0x2df49e0 (the mixer hand-off), +0x50 0x2df4b14 (render gate), +0x58 0x2e36068,
   +0x60 0x2e36070, +0x68 0x2e36110, +0x70 0x2e35ec8 (mix), +0x78 0x2e35fd8 (silence)`.
  `+0x20` (`0x2df49c4`) is a small function that writes a magic `{7, 0xe302, 7}` struct and returns 1 — a
  device-info getter, not a mixer init.

So the object graph is complete and correct: the engine has a device, the device has a sink, and the sink has a
working mixer hand-off entry. Nothing ever calls it, and no plugin is registered. Two gates remain worth
attacking: who sets `[0x430a1cd]`, and what is supposed to register the mixer plugin array `[0x430a250]`.

## Audio, round 18 — the device mix loop runs, the voice pool is empty, and the event names decode

Added five env-gated probes (`games/pla/runtime/audio_sdk.c`) and one behavioural experiment.

**The device mix function `. 0x2e17678` is called every tick and bails out on an empty voice array.** Hook
`PLA_AUDIO_SNDCHAIN_PROBE` prints `[mixer] mixdev a0=0x1 voices=0 varr=0 gate08=0 gate44=0` at 36–42 Hz: the
`w0&1` gate passes, `[0x430ae08]` (voice/array count) is 0 and `[0x430ae00]` (array base) is 0, so the function
returns at `0x2e1769c` before it can reach the mixer hand-off `sink+0x48` (`0x2df49e0`). The hand-off has never
run in any session so far.

**No voice is ever created.** Four hooks prove it in a 45 s scripted boot and in 6 min of manual play:
`0x2e150f4` (voice allocate), `0x2e14e14` (voice build), `0x2e147fc` and `0x2e2547c` (its two caller
functions) are all at 0 hits, while `.wem` media opens do happen (79 in the manual session). So media gets
prepared/streamed without a voice ever entering the mix list.

**Game-object registration works when the command ring is given time to drain.** The earlier "registration is
a no-op" reading was a measurement bug: the command is queued on `[0x430a128]` and only consumed later.
With a 200-tick wait, `+0x050` (`0x2837328`) goes `pend 0 -> 20` and the table becomes
`buckets=29 objs=1 found_go1=1`. In manual play the game itself had registered 14 objects. (The `+0x050`
thunk is therefore the real register entry; the empty table seen at PostEvent time is just "before the game
registers anything".)

**Event names decode with Wwise's FNV-1a-32** (offset `0x811c9dc5`, prime `0x01000193`, lower-cased name).
Cross-checked against two known names, then used to name the observed IDs:
`0xf0396634 PLAY_UI_MESSAGE`, `0x491de2d2 SET_STATE_GAME_AWAKE`, `0x368e8584 SET_STATE_WEATHER_SILENCE`,
`0x9091d76d SET_STATE_MIXER_SYSTEM`, `0x711212ba SPEAKER_MODE`, `0x1ddbf542 HAND_DOCK_MODE`,
`0xd74dd70f CONFIG_UI_SE`, `0xfcb46a13 CONFIG_UI_BGM`.

Net state: events are posted with real names, game objects do get registered, media is opened, the render loop
and the device pump both run — but the voice array the mixer consumes stays empty, so `[fill] mix` never
executes and `[pcmfed] nonzero` remains 0/N.

## Audio, round 19 — the play action never reaches the engine, and media opens only when a human plays

Static follow-through of `CActionPlay` (`0x2de13f4`, the event action that *does* execute: 20–40 hits in manual
play) and of the play-request pipeline:

- `0x2de160c bl 0x2e0f0a8` submits the request. `0x2e0f0a8` (size 0x370) contains exactly **one** `bl`
  (`0x32a3040`, a small SDK/timing helper) and no indirect dispatch: submitting a play request is pure
  bookkeeping inside the game-side queue `[base+0x430a150]`.
- The per-frame drain `0x2e0f418` (size 0x29c, called twice per render tick for the two vectors) only has one
  `bl` (`0x2e0e5cc`) plus four `blr [x23+0x150]` (the vector's free callback). Its state machine is
  `1 -> tick`, `2 -> store frame`, `4 -> accumulate frames, back to 1`, `6 -> release + erase`; it never calls
  into the AK engine.
- The one virtual dispatch in the chain is inside `0x2e0e5cc`: `ldr x0,[req+0x28]` (the first attached action
  object) -> `ldr x8,[x0]` -> `ldr x8,[x8]` -> `blr x8` with `x1 = req`. That slot-0 method is
  `0x2dde98c`, which reads/​writes parameter values (`0x2de3a38`, `0x2de1bec`, `0x2ddf018`) — a game-object
  parameter update, not a voice start.
- Voice creation therefore hangs off a different entry: `0x2e150f4` (the voice allocator) is only reachable via
  `0x2e14e14` (`0x2e14ec8`) and `0x2e15084`, whose callers are `0x2e147fc` / `0x2e2547c`; walking further up
  reaches `0x2e14074` (11 call sites: `0x2da1090…0x2dd4bd4`, `0x2df29d8…`) and `0x2e15a38`, which the device
  pump (`0x2e16f70`) calls every tick but which early-returns because its list `[0x430ae30]` has count 0.
- `.wem` media opens were **not reproducible in a scripted run** (120 s with the same input script that reaches
  the field: 0 `.wem`, 0 `.pck`), only in manual play (79 opens). So the media-prepare path needs real
  interaction; scripted verification cannot reach it.

Conclusion: the play action, the request object and the queue are all game-side/Wwise bookkeeping that never
reaches the engine's voice manager. The missing piece is the "start voice" hand-off — either
`0x2e147fc`/`0x2e2547c` (whose callers include the AK bus/effect region `0x2da1xxx` and the pump) or an
engine entry the port never drives.

### Audio, round 20 — where the game's own sound-start entry sits

The play-request holder `[base+0x430a150]` is touched by 40 functions. Three of them are the game's own sound
API layer and they all funnel into the same request constructor `0x2e0ed58`:

    0x2e06330 (0x858) -> 0x2e067d0   the game's "start sound by id"; its only callers are 0x2d7f394 and 0x2d8ab7c
    0x2e3bf24 / 0x2e3c048 / 0x2e3c1fc / 0x2e3c2d4 -> 0x2e3bfc0/0x2e3c0c8/0x2e3c26c/0x2e3c358
    0x2e445d0 (0x288) and 0x2e44af8 (0x3ec)        the higher game sound wrappers (0x2e44874/0x2e449cc, 0x2e43794)

`0x2e0ed58` itself has eleven callers, and only one of them is the action executor (`0x2de14d4`). The others are
`0x2da21d0/0x2da23d0` (AK bus region), `0x2dcc95c/0x2dd5f20/0x2dd6114`, and the game layer above. So the
"request created" step is reachable from both sides and is not the gap.

Two further facts from this round:

- `0x2e13adc` is *voice re-initialisation from the pool*, not a list insert: it loads `[0x430ae00]`, installs the
  voice vtable `0x41ac6b8` into `[x20]` and `0x41ac6b8+0x150` into `[x20+0xa8]`, then calls `0x2e24810` /
  `0x2e26970`. It is the pool's "reuse this slot" path, so the activity list `[0x430ae30]` is filled by someone
  else (candidates `0x2e1453c`, `0x2e14afc`, `0x2e17da8`, `0x2e18930`; the last one is the only one reachable
  through a relocation, slot `0x41abeb0`).
- `TOTK_AUTOLOAD=1` cannot stand in for a human session here: it presses A only once `save:/slot_NN/caption.sav`
  is opened, and this install keeps the save as a single `user-data/save/<id>/main` snapshot, so the auto-loader
  stays idle (`[autoload] on:` and no press log). A scripted dialog session therefore still needs the user.

Next: resolve the owner of the `0x41abeb0` slot (the class whose instance inserts into `[0x430ae30]`), and diff
that class against a working Wwise title in this tree before touching the engine side again.

### Audio, round 21 — the play request does reach an AK source object

Instrumenting the request's own per-frame tick (`0x2e0e5cc`) settled the shape of a play request. A request is
created, stays in state 1 for its whole lifetime, and **has exactly one source object**:

    [reqtick] req=0x…  state=1 nsrc=1 sid=317282339 srcvt=main+0x41ab9a0
              drv=0x10045c3a258 drvvt=main+0x41aaef8 drvfl=1

`srcvt=main+0x41ab9a0` is the game's sound-source class; its slot 0 is `0x2e05638`. That function looks up the
source id in a key table and, when `[driver+8]` is nonzero, calls `driver->vtable[0x48]`:

    0x2e056c0  ldr  x0,[x19,#0x18]      ; the driver object
    0x2e056c4  ldrb w8,[x0,#8]          ; "started" flag
    0x2e056c8  cbz  w8,0x2e056e0        ; 0 → nothing reaches the engine at all
    0x2e056dc  blr  x8                   ; vtable[0x48]

In the measured run `drvfl=1`, so the flag is not the gate: every frame the game calls into a driver object whose
vtable is `main+0x41aaef8`, i.e. **an object inside the Wwise 2021 C++ region** (`0x2dcf***` – `0x2dd****`), and its
`+0x48` is `0x2dcfcbc`. That function forwards to `[driver+0x10]` and reads the count at `[[driver+0x10]+0x18]`
before deciding whether to walk it (`cbz` when the count is zero and `w4` has bit 0 clear).

So the chain now reads: event → action → game play request → **AK source object → AK driver object** → (count of the
driver's sub-objects is the next thing to measure). The remaining question is narrower than before: does
`0x2dcfcbc` ever get called with a non-empty sub-object list, and what creates those sub-objects?

Probe added: `reqtick` (site `0x2e0e5cc`, `AU_FLOAT_PROBE`) prints state, source count, source id, source vtable,
driver pointer, driver vtable and the driver flag. `TOTK_AUTOLOAD` remains unusable as a scripted substitute
(single-file save snapshot, so no `save:/slot_NN/caption.sav` is ever opened).

Round 21b: the AK driver we reach has an empty sub-object list. Two probes settle it on a scripted run:

    [srcupd] src=0x10045c03960 drv=0x10045c3a258 drvvt=main+0x41aaef8 drvfl=1 caller main+0x2e0e6d8
    [drvupd] drv=0x10045c3a258 sub=0x10045c0fbf0 subvt=main+0x0 sub_n=0

`drvupd` hooks `0x2dcfcbc` (the `vtable[0x48]` the source tick calls). It loads `[drv+0x10]` and reads the count at
`[sub+0x18]`, which is **0** for every driver of every request. That function's first test is exactly that count
(`cbnz w8` → walk the list), so with 0 it returns without mixing anything.

So the whole game-side path is alive and the AK objects it owns are simply empty: the step that would create the
engine-side voice/source entries (the five play-request methods at vtable `0x41aaa60` + 0x20/+0x40/+0x60/+0x80/+0xa0,
all of which forward to `engine->vtable + 0xc0`) is never taken. Everything upstream of that call is now accounted
for, and that single missing dispatch is the remaining defect.

Round 21c: what those five request methods actually delegate to.

`PLA_DUMP_ENG` now prints the singleton the play-request interface methods fetch (TLS slot 0x20, seeded from
`[base+0x42f1798]`):

    [engdump] reqdrv=[42f1798]=0x10ecf7fa8 vt=main+0x40c2e00 vt+0x00=main+0x84c438 vt+0xc0=main+0x84bf34

and `0x84bf34` is literally `mov w0,#1; ret`. So the five "forward to engine" methods are not a playback path at
all: they ask the game's own sound-device interface a question and always get 1 back. That also means hooking them
(round 18) could never have produced audio, and the game-side bookkeeping genuinely is bookkeeping.

`vt+0x00` (`0x84c438`) is that interface's constructor (installs the table at `[base+0x40c2e00]` and a second one at
`+0xe8`, then calls `0x32a4610`), so `0x40c2e00` is a deployed function table, not a C++ vtable.

Consequence: the only remaining live path is source tick `0x2e05638` → AK driver `vtable[0x48]`
(`0x2dcfcbc`) → `[driver+0x10]` source list, whose length is 0. Nothing in the game-side chain creates entries
there, so the next question is what AK call registers a source into that driver, and which game path was meant to
issue it.

Round 22: the game-object table is NOT empty in a loaded-save session, and an automated field harness exists.

Fixing the `postevent` probe to read the same table object the dispatch handlers use (`[base+0x430a140]`, whose
bucket count is at `+0x38`) changes the picture:

  title/menu session:  [sndchain] postevent #1 eventID=0x491de2d2 go=0x1 gobuckets=0  goslot_nonzero=0
  loaded-save session: [sndchain] postevent #22 eventID=0xcb4875d9 go=0x1 gobuckets=29 goslot_nonzero=0

So after a save is loaded the game-object table does have 29 buckets; the previous "table is always empty, every
event is dropped" statement only holds for sessions that never get past the title screen.

Also new: `/tmp/pla_re/inp_load3.txt` (long warm-up + A presses) plus `TOTK_SWKBD_ANSWER` loads the existing save
without a human, reaching `帧 6300` with `signal=0`. In that same session the play-side sites are all zero
(`actplay/actlookup/post1/actgo = 0`) while 24 events fire, and the AK driver update still reports
`sub_n=0` for every driver (`[drvupd] #4800 drv=… sub=… sub_n=0 w1=16908327 caller main+0x2e056e0`).

Next: instrument the three-point funnel (event job enqueue → ring dispatch → action execution) in one automated
field session so "enqueued vs dispatched vs dropped" becomes a measured number instead of an inference.

Round 23: registering the game object is NOT the blocker, and the play branch is essentially never entered.

The `postevent` probe now walks the whole game-object table instead of peeking at one bucket:

    [sndchain] postevent #1  eventID=0x491de2d2 go=0x1 gobuckets=0  gobjects=0  gofound=0
    [sndchain] postevent #20 eventID=0xc90af43b go=0x1 gobuckets=29 gobjects=13 gofound=0

So a loaded-save session does populate the table (13 objects) but **never registers the id every event targets (`go=1`)**. Feeding
the engine's own register command for id 1 (`PLA_REG_GO_RT=1`, which reports `buckets=29 objs=1 found_go1=1`) does *not*
change anything: the events still produce no playback action, `[fill] mix` stays 0 and `[pcmfed]` stays zero.

With the AK event-manager's type-1 handler instrumented (`0x2d7ed80` → `evdisp`, plus its callees `0x2d81050` `execon`
and `0x2d813f0` `exeplay`), an automated loaded-save session shows `evdisp=1`, `execon=0`, `exeplay=0` while 24 events
are posted. The engine therefore barely enters the play branch at all, and never reaches the two functions that lead to
`CActionPlay` → play request → source tick → driver.

Reading: the missing piece is not "the game object is unregistered" but "the events that should play are not the events
being posted" — in automated sessions the game posts state/config events (`SET_STATE_*`, `SPEAKER_MODE`,
`CONFIG_UI_*`), and the play events only appear when a human drives real menus/dialogue.

Round 24: an instrumented play branch, and a limit found in the automated harness.

Probes added for the AK event manager's own handler chain — `0x2d7ed80` (`evdisp`, type-1 "play on game object"),
`0x2d81050` (`execon`) and `0x2d813f0` (`exeplay`) — plus a fix so `evdisp` reads the event's game-object id at the
message offset that `0x2d7ec5c` uses (`+4`, not `+0xc`).

In automated loaded-save sessions the funnel reads:

    postevent 23-24   evdisp 1   execon 0   exeplay 0   actplay 0   post1 0

i.e. the engine enters its play branch once per session while the game posts ~24 state/config events, and never
reaches the two functions that lead to the game's own `CActionPlay`.

Two automation results worth recording so they are not repeated:
* `inp_ui.txt` / `inp_menu.txt` (A/Plus cycles after a fixed warm-up) do not produce play events, even when the
  session reaches `帧 5400`-`7500`.
* key spam from the very first frame stops progress: `spam1` reached `帧 1200` in 300 s with `[pcmfed] append #8`
  and `mixdev 0`, i.e. the audio device never started. Long idle first (`inp_spam2.txt`, 220 s idle) recovers the
  frame rate (`帧 7500`) but that run's device likewise never left its initial 8 appends.

Static lead for the next session: `PostEvent` hands its work to the engine job manager at `[engine+0x170]`
(`0x2d77020`: `ldr x23,[0x430a120]` … `ldr x0,[x8,#0x170]`), and `0x2ddcc24` then allocates the 0x34-byte action
from the pool. Whether that job queue is ever drained is the only remaining invisible step between "event accepted"
and "play branch entered", so the next probe belongs on the job consumer.

Round 25: what `0x2ddcc24` really does, and a first look at the pending-action map.

Disassembling the function `PostEvent` calls changes its role: `0x2ddcc24` is not a queue push but a **hash-map
insert** into the object at global `0x430a170` (loaded as `ldr x0,[x8,#0x170]` with `x8 = 0x430a000`, i.e. the global
itself, not `engine+0x170`):

* `[map+0]` bucket array, `[map+8]` bucket count, `[map+0x10]` live count, `[map+0x18]` load-factor threshold;
* it computes `bucket = action->hash(+0x4c) % count`, prepends the action (`[action+0x68] = old head`), increments
  `[map+0x10]`, and only when the load factor stays under `0.9` (`0x3f666666`) does it keep the small table;
  otherwise it falls into the resize path.

`PLA_DUMP_ENG` now also prints that map. First sample (30 s, before any play event):

    [engdump] actionmap=0x10044ff31e0 buckets=11 live=0 lf=257 entries=0

so pending actions are not accumulating at that moment — the map is empty, which means either the actions are
executed promptly or they never get inserted (the 24 state/config events resolve instantly). The next sample must be
taken *after* a session that actually posts play events; today's runs could not provide one: the automated session
stalled at `frame 77` with the known `mtl_link_wait_tick` / `h_nvnWindowAcquireTexture` thread dumps, so the dump
only ever ran once.

Round 26: the pending-action map is never drained — measured, not inferred.

`PLA_DUMP_ENG` now snapshots the map (global `[base+0x430a170]`, layout from `0x2ddcc24`), and the PostEvent probe
takes that snapshot right after every accepted event. In a loaded-save session the live count rises strictly
monotonically and never comes back down:

    after postevent #1  buckets=11 live=0  lf=257 entries=0
    after postevent #2  buckets=11 live=1  lf=257 entries=1
    after postevent #3  buckets=11 live=2  lf=257 entries=2
    ...
    after postevent #11 buckets=11 live=10 lf=257 entries=10
    after postevent #12 buckets=29 live=11 lf=257 entries=11   <- table grew (load factor reached)
    ...
    after postevent #20 buckets=29 live=19 lf=257 entries=19

20 posted events, 19 actions still pending, zero executed. Feeding the engine its own register-game-object command
does not change it, and that run also shows the ring behind `[base+0x430a128]` accumulating
(`pend_before=5200 → pend_after=5220 → 6020` two hundred ticks later, while the registration never applies because
its command is stuck in the same backlog).

So the missing piece is a **consumer**: something has to take actions out of that map (and commands out of that ring)
and execute them. The engine creates exactly four helper threads (`AK Suspended`, `AK::BankManager`,
`AK::EventManager`, `AK::OpusDecodingThread` — via the only callers of the thread helper `0x2d717c0`), and none of
them drains these structures in the sessions we can run. The render body `0x2d7e250` only executes the command path
when global `[0x430a248]` (command count) is non-zero, and that count is always 0.

Next: hook the remaining ring users (`0x2e28618` 11 refs, `0x2e28c70/0x2e28d14/0x2e28dc0/0x2e28f88`, `0x2e17a04`,
`0x2e29984`, `0x2e330d4` = EventManager loop) and count which of them run at all — the one that never runs is the
missing consumer, and if all of them run, the backlog is a priority/ordering bug inside the queue instead.

Round 27: none of the seven ring users ever run, and the action map grew to 20.

Probes were added for every remaining function that touches the job/command ring at `[base+0x430a128]`:
`0x2e28618` (`ringA`, 11 references), `0x2e28c70`/`0x2e28d14`/`0x2e28dc0`/`0x2e28f88` (`ringB`..`ringE`),
`0x2e17a04` (`ringF`), `0x2e29984` (`ringG`). In a loaded-save session that reached `帧 1500`:

    ringA 0   ringB 0   ringC 0   ringD 0   ringE 0   ringF 0   ringG 0
    after postevent #26 buckets=29 live=20 entries=20
    after postevent #27 buckets=29 live=20 entries=20

Two readings matter. First, **none of those seven functions execute at all**, so whichever of them owns the drain is
not running. Second, the live count is *flat* across #26 → #27 (20 → 20) after rising monotonically to 20 earlier —
i.e. actions are occasionally removed, so a consumer does run sometimes; it just does not scale with the events the
game posts (one action removed versus two dozen queued).

That shifts the question from "is there a consumer thread" to "why does the drain run so rarely", which points at the
per-game-object tick: actions sit in the map until the *target game object* is ticked, and the game posts to `go=1`,
an id the game never registers. Registering it from the host does not help because the register command itself waits
in the same backlog.

Round 28: the job ring is drained after all — my earlier "never drained" reading was a misparse.

Sampling the object at `[base+0x430a128]` from the host (three points in one loaded-save session):

    [ringq] t=30  ring=0x10044fec340 rd=0x10045000dd8 wr=0x10045000dd8 cap=0x10045041100 pend=0
    [ringq] t=90  ring=0x10044fec340 rd=0x10045000e08 wr=0x10045000e08 cap=0x10045041100 pend=0
    [ringq] t=150 ring=0x10044fec340 rd=0x10045000e08 wr=0x10045000e08 cap=0x10045041100 pend=0

`+0` and `+8` are **data pointers, not indices**; read == write at every sample, so the queue is empty and the
consumer is keeping up (the write pointer even advanced between t=30 and t=90, i.e. commands were posted *and*
collected in that window). The earlier "pend 5200 → 6020" reading came from reading this structure before it was
initialised, and the conclusion built on it ("nobody drains the command ring") is withdrawn.

That removes the last "missing consumer" candidate. What is still measured and still wrong:

* the pending-action hash at global `[base+0x430a170]` accumulates: 59 events → 27 live actions, occasionally
  plateauing (20 → 20, 27 → 27) rather than falling;
* no voice is ever created (`[0x430ae00]`/`[0x430ae08]` stay 0, `0x2e150f4`/`0x2e14e14` never run);
* the device mix loop runs every tick but returns early on the empty voice list.

So the question narrows to: **what takes an action out of the pending map** (it is not the command ring), and why does
that path see ~0.5 actions/tick instead of one per event.

Round 29: every remaining "consumer" candidate is dead, and SoundEngine::Init runs exactly once.

Three more rounds of probes, all of them counting zero:

| probe | target | role | hits |
|---|---|---|---|
| `drainA..G` | `0x2db69d8`, `0x2dd184c`, `0x2daafd0`, `0x2dabb70`, `0x2dabf78`, `0x2dd41a8`, `0x2d8709c` | every function that writes the job ring's read/write words | 0 |
| `mapA..E` | `0x2e16c00`, `0x2e18c50`, `0x2e1c9a0`, `0x2e1e720`, `0x2e30254` | functions referencing the `[base+0x430a170]` job manager | 0 |
| `akinit` / `akterm` | `0x2d74ee0` / `0x2d758c0` | `AK::SoundEngine::Init` / `Term` | 1 / 0 |

So the engine is initialised once and never torn down — the "it re-inits and loses voices" theory is dead too, and none of the 12 functions that touch the ring or the job manager ever execute.

Sampling the job manager from the host at t = 30…180 s in two different automated sessions shows the object present but its table never
built (`buckets=0 live=0`), i.e. those sessions post no events; the accumulating-action readings only ever came from sessions that
reached real gameplay (`帧 4800+`, `.wem` opens), so the measurement has to be taken there.

Next: one long automated run that reaches the field (the `TOTK_SWKBD_ANSWER` + `inp_auto.txt` path, which did reach `帧 4800` and open
`.wem` files before), sampling the job-manager table each event to see whether `live` ever falls, and hooking the *event-definition*
lookup path instead of assuming the job manager is the pending-action store.

Round 30: the action map drains at the event rate — so the queue is not the blocker, the never-built voice manager is.

A 4-minute automated run (25 `PostEvent`s, `帧 3000`) sampled the job-manager table after every event:

    after postevent #22 buckets=29 live=20 entries=20
    after postevent #23 buckets=29 live=20 entries=20
    after postevent #24 buckets=29 live=20 entries=20

`live` stops rising even though events keep arriving, so entries *are* being removed at roughly the arrival rate. The earlier
"actions pile up and are never taken" reading was a startup transient: the count rises to ~20 and then stays flat. The pending
queue is therefore not the blocker and the whole "missing consumer" line of investigation is closed.

What the same runs do show, and what now looks like the actual cause:

* play actions execute (`CActionPlay 0x2de13f4` fires 20–40× in real sessions) and produce play requests;
* a request reaches its AK source (`srcvt=main+0x41ab9a0`) and the source calls `driver->vtable[0x48]` (`0x2dcfcbc`);
* that function immediately returns because the driver's sub-object list is empty — and the sub-object itself is **uninitialised**:
  `sub=0x10045c0fbf0 subvt=main+0x0 sub_n=0` (a null vtable), i.e. the voice manager hanging off `[driver+0x10]` was never constructed;
* no voice is ever created (`0x2e150f4`/`0x2e14e14` hit 0), so the device mix loop always finds an empty voice list and renders silence.

So the remaining question is concrete: **who constructs `[driver+0x10]`'s sub-object, and why does that constructor never run in this
port** — with the standing suspicion that it belongs to the game-side scene sound manager (`0x1dd7c68` → `0x1dd82b8` → `0x854984`),
whose chain we measured as almost entirely unhit (`famsib=1 sndtop=6 sndbig=1`, everything else 0).

Round 31: play events can now be posted on demand from the guest thread (and the two "ring" readings disagree).

New diagnostic: `PLA_TEST_PLAY=<frame>:<EVENT>[,<EVENT>...]` posts a real Wwise event from the AK render hook
(`au_render_trace_note`, which runs on the guest AK thread, where PostEvent is the normal thing to call — the earlier
host-thread and loader-stage attempts both deadlocked/stalled).  Verified:

    [testplay] armed: post "PLAY_UI_MESSAGE" at frame 300
    [testplay] PostEvent("PLAY_UI_MESSAGE", go=1) -> 6 (n=301)

So the automation gap is closed: a play event can be produced without a human playing.

Combining it with `PLA_REG_GO_RT=200,1` still does not reach voice creation, and this run exposed a contradiction that has
to be settled before anything else:

    [reggort] n=1200 slot+0x050(go=1) -> 1 buckets_before=0 pend_before=5252 pend_after=5292
    [reggort] +200 ticks later: buckets=0 objs=0 found_go1=0 cmd=0 pend=6092

`au_ring_pend()` — the helper used by the registration probe, which reads read/write/capacity from `[base+0x430a128]`'s
object — reports a five-thousand-entry backlog that keeps growing, while the host-side sampler added in round 28 read the
same global and found `rd == wr` (empty).  Both cannot be right: one of the two is reading the wrong object or the wrong
field offsets.  Until that is resolved, neither "the ring is drained" (round 28) nor "nothing drains the ring" (round 27)
can be claimed.

Next: print the raw ring object identity and all six fields it uses from *both* probes in the same run, and pick the reading
that matches the write pointer produced by a known `PLA_REG_GO_RT` post.

Round 32: contradiction settled — the job ring really is drained (round 28 stands, round 31's alarm withdrawn).

One probe now prints every field of the ring object plus `au_ring_pend()` in the same line:

    [ringq] t=30  ring=0x10044fec340 f0=0x10045b530e8 f8=0x10045b530e8 f10=0x10045b53000 f18=0x10045b93000 f20=0x40000 pend=0
    [ringq] t=60  ring=0x10044fec340 f0=0x10045b531c8 f8=0x10045b531c8 f10=0x10045b53000 f18=0x10045b93000 f20=0x40000 pend=0
    [ringq] t=90  ring=0x10044fec340 f0=0x10045b5323c f8=0x10045b5323c f10=0x10045b53000 f18=0x10045b93000 f20=0x40000 pend=0

`+0`/`+8` are the data pointers, equal at every sample, both advancing over time; `+0x10` is the buffer, `+0x20` the
256 KiB capacity. The queue is empty and being consumed, so the `pend ≈ 5000` figures printed by the registration probe
come from a different structure/offset and are not a backlog. Both "who drains the ring" and "the ring is stuck" are now
closed; the ring is neither the problem nor the missing piece.

Where that leaves the investigation: the game posts real events (and now can be made to do so on demand via
`PLA_TEST_PLAY`), the pending-action table drains, but no voice is ever created (`0x2e150f4`/`0x2e14e14` never run) and
the AK game-object table stays at 0 buckets in automated sessions while the game itself builds 29 buckets / 14-18 objects
when a human plays. Programmatic registration through the AK API table (`+0x050`, command type 0xb) is accepted
(`-> 1`) but never creates a bucket, so that entry point is either not "RegisterGameObj" or its command is dropped —
which is the next thing to identify, because the play actions are all targeted at a game object and will not run while
no game object exists.

Round 33: registration + injected play event both work, and the action still never runs.

Two working tools, both verified in automated runs:

* `PLA_REG_GO_RT=<n>,<id>[,<id>...]` — registering through the AK API table slot `+0x050` (`main+0x2837328`) **with a
  non-null debug-name argument** succeeds for real: `[reggort] slot+0x050(go=1) -> 1 buckets_before=0 pend_before=0
  pend_after=20` then `+200 ticks later: buckets=29 objs=1 found_go1=1 ids=1,...`. (Earlier attempts passed `x2 = 0` and
  the engine answered success without creating a bucket — the name argument matters, or the timing did.)
* `PLA_TEST_PLAY=<frame>:<EVENT>` — posts a real event from the AK render hook: `PostEvent("PLAY_UI_MESSAGE", go=1) -> 6`.

Combined, the game object exists and the event is accepted ... and nothing downstream happens:

    [akpath] actplay    #0   (CActionPlay 0x2de13f4)
    [akpath] VOICE CREATE (0x2e150f4)
    [pcmfed] append ... nonzero=0/1800

So the missing link is now precisely **"how does an accepted event's action get executed against a registered game
object"** — not registration, not the event name lookup, not the bank load, not the ring, not the queue drain.

Operational note learned the hard way: the full 15-site probe set perturbs the AK timing enough that the registration
command never reaches the engine (`buckets` stays 0, `pend` shows a backlog). Minimal probe sets are mandatory for any
measurement that involves posting or registering; the minimal set is now `PLA_AUDIO_SNDCHAIN_PROBE=1
PLA_AUDIO_SNDCHAIN_SITES=2` (CActionPlay + voice allocator).

Round 34: the AK low-level engine (the object that owns voices) was never created.

Dumping the whole `0x430ae00`..`0x430ae80` region at runtime gives:

    [engdump] ae: 0 0 0 0 0 0 0 0 0 0 0 0 P P P P
    [engdump] ae+0x60 = 0x10fcbf040   (+0x68/+0x70/+0x78 likewise non-null)

Everything voice related is null:

* `0x430ae00` / `0x430ae08` — the per-device voice list and its count
* `0x430ae30` / `0x430ae38` — the active sound-instance list and its count
* `0x430ae58` — the **head of the LEngine instance list**

`0x430ae58` is a doubly linked list of engine instances (each node uses `+0x48` as `next` and
`+0x198` as `prev`, and `+0x190` as its voice-pipeline object — see the push at `0x2e1958c` inside
`0x2e18c50`, the pop at `0x2e18b30` inside `0x2e18b24`). The per-tick voice services
`0x2e16f70` → `0x2e15a38` do

    0x2e15aa4  ldr x20, [x19, #0xe58]     ; LEngine list head
    0x2e15ab4  cbz x20, 0x2e15b1c         ; empty -> skip every voice service
    0x2e15ab8  ldr x0, [x20, #0x190]
    0x2e15abc  bl  0x2e2547c              ; the only external path to voice creation

and `0x2e2547c` → `0x2e14e14` (build voice) → `0x2e150f4` (allocate voice, the **only** writer of
`0x430ae08`) is therefore unreachable. That single null pointer explains every remaining symptom:
no voices, empty mixer input, silence-only device fills, `nonzero=0`.

Next: find the LEngine constructor (the object whose vtable carries the VPL tick `0x2e14074`) and
create/link it from the port's bootstrap if the game never does.

Round 34b: the engine singleton has no LEngine pointer either.

Scanning the engine object (`[base+0x430a120]`, first 1 KiB) for any pointer whose pointee has a
vtable in the audio-table page finds nothing:

    [lescan] engine glob=0x10045a47000      (and no candidate lines at all)

So this is not "the LEngine exists but is not registered in the list" — the low-level engine object
simply does not exist in this process. Everything downstream (voice pool, mixer hand-off, device
voices) is empty because of that.

The next question is *why*: `AK::SoundEngine::Init` (`0x2d74ee0`) is reached exactly once through the
AK API table thunk `0x283729c` (`mov x0,x1; mov x1,x2; b 0x2d74ee0`), and its AKRESULT was never
inspected. If it returns anything other than AK_Success the engine can still serve banks/events while
the low-level engine stays absent, which matches every measurement we have.

## Audio, round 34d — the two audio paths, and why both stay silent

The engine never creates a sound: after round 34 showed `[0x430ae58]` (the low-level engine list) is
empty and nothing in the engine singleton looks like an engine instance, I went after *why*.

**Path A — our `nn::audio` function overrides (the one a normal build uses).**
The host answers `OpenDefaultAudioOut` / `AppendAudioOutBuffer` / `SetAudioOutBufferInfo` /
`GetAudioOutBufferDataPointer` / `GetReleasedAudioOutBuffer` / `Start`/`Stop`/`Close`/`State`/`Channels`
itself. It works: the sink is created, buffers are appended, the queue plays, the released-get blocks
correctly — but the engine still creates no voice, so every mixed frame is silence.

**Path B — the SDK module's own implementation (`PLA_NO_AUDIO_SDK_OVERRIDE=1`).**
With the `nn::audio` function overrides skipped (but the audio-device overrides kept), the SDK's own
`nn::audio` code runs and talks to the `audout:u`/`audren:u` IPC services the host also emulates — the
path by3 plays Wwise through. Result: `audout:u` opens an output once (`AudioOut open 48000 Hz x2 PCM16
-> Core Audio (stereo)`), `audren:u` is opened by the guest 4 times when the device overrides are also
skipped, but the SDK never appends a buffer and never reaches a voice either; with the device overrides
skipped as well it opens 16 354 outputs with a garbage channel count (0xCB000002), i.e. the SDK's own
device query path is not usable in this port.

So both paths stop before the engine's own voice layer, and the difference between them is where.
by3 does not have this problem because its Wwise drives `audout:u` **directly** (no SDK layer between),
which is why emulating that service is enough there.

Two diagnostic switches were left in place (both default off, both only change behaviour when set):
`PLA_APPEND_BOOL=1` flips `AppendAudioOutBuffer` from the Result-like convention (0 = accepted) to the
bool one nxnative's host answers (1 = accepted); `PLA_NO_AUDIO_SDK_OVERRIDE=1` skips the `nn::audio`
function overrides so the SDK path can be compared.

Next: use path B with `audout:u` tracing to see the SDK's own call sequence, and compare it with by3's
working order (ListAudioOuts -> OpenAudioOut -> RegisterBufferEvent -> Start -> Append), because the
switch that decides whether the engine starts voices is upstream of everything the host controls today.

## Audio, round 35: the silent port was a trailing slash

- **The root cause of the whole silence**: Wwise checks that its base path `data:/bin/sound/NX64/wwise/bank/` (with a trailing `/`) is a directory before it starts the sound engine. `runtime_romfs_entry_type()` (`runtime/romfs.c`) resolved the trailing-slash path through `file_index()` (not a file), then appended another `/` and asked `folder_exists()` for `.../bank//` — so `nn::fs::GetEntryType` answered `PATH_NOT_FOUND` and the sound engine stopped right there. Everything downstream (banks never loaded, game objects never registered, actions accepted but never executed, no voice, mixer fed silence) followed from that one answer. Same defect and same fix as `games/pokesv` (Violet, CHANGELOG 164): a folder may be named with one trailing `/`.
- `runtime_romfs_entry_type()` now trims one trailing `/` (not for `x:/`), remembers the path is folder-only and skips the file lookup; a file named that way is still not found.
- `PLA_FS_ET_TRACE=1` logs every `GetEntryType` path and its result (`[fs-et]`), which is how the check was caught: `[fs-et] "data:/bin/sound/NX64/wwise/bank/" -> romfs -> 0 type=0`.
- First sound: `[fill] mix` runs (13 times in 110 s), `[pcmfed] append #4800 ... nonzero=4495`, i.e. the mix path produces samples. Audible, but noisy (open).

## Audio, round 36: the crackle was the queue draining every 85 ms

- With the engine finally running, the played audio crackled. `[aq]` tracing (`PLA_AQ_TRACE=1`, time-stamped
  enqueue/played lines with queue depth) showed the cause: the game owns four 1024-frame buffers (85 ms), and it
  only rendered the next burst once all four had been played, so the Core Audio queue fell to **depth 0** on every
  burst (depth sequence 3,2,1,0 repeating) — a gap of the game's render latency at 85 ms intervals, heard as noise.
- `PLA_AUDIO_LEAD=n` (default 6): a tag is handed back as soon as its samples are copied while the device is still
  `n` buffers behind, instead of waiting for `played`. The samples are copied before the tag is returned, so the
  game may safely refill the buffer. `Queued::prerel` keeps `release_head` from returning a tag twice.
- Measured before/after (`PLA_AQ_TRACE=1`, 60 s automatic run): depth 3,2,1,0 repeating → depth 3–6, `depth=0`
  only 5 times at start-up. Appends stay at 45.9/s (real time), so no runaway.
- `PLA_AUDOUT_LIVE=1` counts live AudioOut objects: six are opened over a session and none is closed; five hold only
  their four start-up (silence) buffers, the sixth carries the whole mix (2744 appends). Harmless but not tidied.

## Stability, round 1 — A1: a memory pool is guest accounting, not a GPU resource

- The P0 crash (`[nvn-mtl] FATAL 内存池 … 建零拷贝缓冲失败` → `abort`, seen at frame 4500 and at frame 600) was the
  pool mapping, not the game: `mtl_pool_add()` created one `newBufferWithBytesNoCopy` **per NVN memory pool at
  Initialize time** and held it until `nvnMemoryPoolFinalize`, which the game almost never calls. Measured: 922 pools
  alive by frame 2400, plateauing there — right at the point where Metal started refusing new no-copy buffers.
- Fix (`third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`, `nvn_pool_index.h`): a pool is registered as a range only; its
  zero-copy buffer is created on **first GPU address** (`pool_buf_get` from `mtl_addr` / `mtl_gpu_addr` / `pool_res`),
  i.e. only for pools the GPU actually reads. A re-initialised pool whose range is already registered reuses the
  existing entry instead of asking Metal for a second buffer over the same pages (that refusal was the abort). A
  failure now drains the GPU and retries once, then logs the device numbers and treats the pool as "not addressable"
  instead of ending the session. `TOTK_POOL_LOG=1` adds a line per pool Initialize/Finalize.
- Measured, same automated run (`inp_auto`, 420 s, `TOTK_GPU_ALLOW_SUBMIT=1`):

  | frame | pools | zero-copy buffers before → after | 显卡已分配 |
  |---|---|---|---|
  | 300 | 156 | 156 → **31** | 997 → **678 MB** |
  | 2400 | 922 | 922 → **206** | 1428 → **931 MB** |
  | 4800 | 922 | — | 931 MB, `gfx` healthy, **no FATAL** |

  The session now runs straight past frame 4500, where it used to abort.
- `内存 至帧` now also prints the live zero-copy buffer count, so pool growth and buffer growth can be told apart.

## Picture, round 1 — key 3: MetalFX output upscaling

- `runtime_output_mode_cycle()` was a stub (`return 0`), so key 3 did nothing. PLA's canvas and output size are baked
  into the game (1920x1080; no patch of ours changes them), so the picture switch this port can offer is the present:
  `mfx_output` + `mfx_upscale` off = 1:1 blit, on = the MetalFX output scaler draws the 1080p picture at the window size.
- Measured with both flags on (window 3014x1688, scale 2): `[mfx-up] output: 1920x1080 -> 3014x1688`, 1200 upscales over
  1218 frames, GPU **p50 0.76 / p95 0.77 ms** per upscale; the scaler itself is built in the background (67 ms) so the
  first frames keep the bilinear present. The HUD already reads this state ("画质：MetalFX 2K"), so no UI work was needed.
- Key 2 (`mfx_interp`) already cycles its four modes and now reports them (`30+30`, `half the refresh rate`, ...), but
  interpolation inserts **0** frames in PLA: `[mfx] 至帧 1649：插值帧 0 / 游戏帧 300 …未插：缺输入 300（画面 300 相机
  300 深度 300 UI 300）`. The MetalFX input capture in `nvn_mtl.m` is written against TotK's pass structure — its UI
  vertex shaders, an 8-attachment canvas pass and TotK's 84-float camera block — and none of those hooks fire for PLA:
  the canvas is never learned, `mfx_note_scene` is never reached, and the camera block is never recorded. Making key 2
  actually double the frame rate needs PLA's scene/UI boundary and camera block identified first (next step).

## Picture, round 2 — B1: PLA's camera block, so MetalFX interpolation runs

- Round 1's "interpolation inserts 0 frames" is fixed. Two things were wrong, and neither was MetalFX.
- **The canvas was learned twice.** The composite rule (`vertex shader ac195790`) does learn PLA's canvas (texture 3896),
  but `canvas_learn()`'s swapchain heuristic re-learned it from the last canvas-sized target on every frame — the swap
  copy (3900/3901) — so at the UI draw `g_mfx.canvas` no longer matched the render target: the scene copy never ran and
  the UI input stayed missing, while the camera input inherited the same frame. The composite rule now locks the canvas
  (`g_mfx.pla_canvas`); the heuristic only runs until the composite has been seen.
- **PLA has no 2368-byte TotK camera block.** Its camera is the vertex uniform at bind 1, **448 bytes** (112 floats):
  three column-major 4x4 matrices — VP at float 0, view at 16, projection at 32. Layout measured from a
  `PLA_UBO_DUMP=bee0581a` dump at game frame 141: `P_cm * V_cm == VP_cm` to 9e-8.
  PLA's projection is **reversed-Z with z in [0,1]** (`z_ndc = (a·z + b)/(-z)`), and both `near/far` can be derived
  instead of assumed: `a = P[2][2] = near/(far-near)`, `b = P[2][3] = near·far/(far-near)` → `near = b/(1+a)`,
  `far = b/a`. The first dump was a 120 m cutscene camera, the real title/game camera reports `far = 65535`, so an
  early `a > 1e-6` sanity check rejected every real frame (`0.01/65535 = 1.5e-7`); it is now `a > 1e-9, far < 1e6`.
  Position is `-Rᵀ·t` of the view matrix, the view z axis is view column 2 (`v[2], v[6], v[10]`).
- **Depth convention is a parameter now.** PLA's depth is reversed with z in [0,1]; TotK's is 2·d-1. `totk_mfx.m`
  gained `mfx_set_depth_convention(zscale, zoff, reversed)` (kernel `zscale/zoff` params, `fi.depthReversed`), and the
  PLA camera path sets `(1, 0, reversed=1)` once. `nvn_obj.h` selects the decoder with `MFX_CAM_BLOCK_PLA`
  (`MFX_CAM_SIZE 448` on the PLA branch; TotK's path is untouched).
- Measured (`TOTK_MFX=interp30`, 20 s, title scene, `TOTK_WINDOW_SHOT=100,250`):
  `插值帧 284 / 游戏帧 300`, MetalFX GPU **p50 3.41 / p95 3.70 ms**, unmatched input 14 (all of them the first frames
  and the UI layer), camera/深度 misses 0. The full-window shot shows the title screen with correct colour, no white
  wash, no ghosting, and crisp UI/logo. Four modes cycle from key 2 with the pacing line each time
  (`30+30` / `half the refresh rate` / `uncapped` / `off`, and back).
- Still open for B1: motion-heavy gameplay (the title camera barely moves), `TOTK_MFX_MVLOG` sanity on the motion
  vectors, and audio-sync checks while interpolating. The camera cut heuristic (8 m / 25°) now runs on real PLA data.
- **Frame-rate measurements need an idle machine.** Every automated run here measured ~62 ms/frame (15 fps) even though
  the same build at 19:41 measured 33.33 ms. Ruled out: `DM_PRESENT_LINK=0` (62.21 ms), no GPU submit (64.07 ms),
  the per-frame TSV (`@gpu_ms` 2.6, `@proc_cpu_ms` 9, `@wait_ms` 8), and finally the code itself — stashing all B1
  changes and rebuilding pristine `HEAD` gave `@frame_ms p50 62.08`. The machine was busy with the user's apps (GPU
  device utilisation 75-88 %, WindowServer 39 % CPU), so 62 ms is the machine, not the port. A6's p95 gate and any
  frame-rate comparison must be measured with the machine idle.

## Picture, round 3 — the interpolated frame's colour (the exposure flicker)

Round 2 got MetalFX interpolation running, but on screen the picture flickered: the interpolated frames were brighter than
the game's own frames, so at 60 Hz output it read as an exposure flash twice a frame. Measured on the title screen, mean
luma over the 1920x1080 picture:

| frame | before | after |
| --- | --- | --- |
| real (game) frames 200/205/210 | 191.56 / 191.01 / 190.75 | 191.72 / 191.07 |
| interpolated frames 200/205/210 | 222.54 / 221.65 / 221.08 | 192.20 / 191.31 |

- **The canvas was innocent.** Decoding the dumps (`TOTK_MFX_DUMP`): the canvas copies are `RG11B10Float` display-referred
  linear values (encoded: luma 193.3) and the game's swapchain frame is 188.1 — the same picture within a rounding.
- **`MTLFXFrameInterpolator` has no colour-processing mode.** Unlike `MTLFXTemporalScaler`/`MTLFXSpatialScaler`
  (`colorProcessingMode` = perceptual/linear/HDR, which `totk_mfx_up.m` uses), the frame interpolator descriptor only
  takes texture formats. It writes *display-space* bytes: with an `RGBA8Unorm_sRGB` output texture the hardware encoded
  them a second time — sRGB(sRGB(L)) — which is exactly the washed-out, raised-black look we saw (out luma 219.6).
- **Fix:** keep the display bytes in a plain `RGBA8Unorm` (`ensure_out()`), present through an **sRGB view**
  (`g.outv[i] = [g.out[i] newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB]`, returned by `mfx_output()`), and
  let the sRGB drawable encode once. One decode, one encode, net identity. `TOTK_MFX_SRGB` (the experiment from the
  previous round) is gone with the guesswork.
- Diagnostic that made it measurable: `TOTK_MFX_DUMP_ANY=<n>` forces a dump every n interpolated frames; without it
  `mfx_dump_due()` only dumps while the camera turns, and the title screen never does.

Verified: interpolated frame PNG within 0.5 luma level of the real frame, dumped `out` bytes equal to the real swapchain
frame (188.0 vs 188.1), and the picture visually identical (black text stays black). Gameplay motion, ghosting and audio
sync still need a scene with a moving camera.

## Picture, round 4 — the interpolation modes a delivery build can land on (2026-11-03)

The user set the scope: native 60 fps is the default deliverable, and interpolation only has to support 30+30. Four modes
were reachable from key 2, and two of them (half refresh, uncapped) are calibrated for a 100 Hz display, not this port.
`totk_mfx.m` `mfx_set()` now cycles **off <-> 30+30 only**; `MFX_HALF`/`MFX_MAX` stay reachable through `TOTK_MFX=interp`
/ `interpmax` for diagnostics, but a delivery build cannot land on them by accident. An explicit `mfx_interp = 2/3` is
clamped to 30+30.

Verified (`two1` run, `TOTK_INPUT_FILE` pressing key 2 three times): `[flag] mfx_interp = 1` -> `0` -> `1`, with
`出图节拍切换：补帧 30+30` / `补帧关` each time and never mode 2 or 3; the same run still produced interpolated frames.

## Performance, round 1 — native 60 fps: the game's own frame step

PLA locks itself to 30 fps by asking the window for a present interval of 2; the simulation is advanced by a float the
main loop computes from a clock object (`main+0x27814b4`, ns / 1e9) and hands to `TaskManager::Update`
(`main+0x282841c`, whose task timers subtract it). Presenting every display tick without touching that float would run
the game at double speed, so the port does both halves:

- **Frame step**: 7 words at `main+0x27814b4..0x27814cc` become `adrp x9,<page>; ldr s0,[x9,#4]; 5×nop`; the host
  writes the measured present interval into that page (low float = step×30, high float = step in seconds, 8-byte
  atomic word). Data page is the loader's reserved writable page after the image (`0x4554000`), checked to carry no
  relocation. All 7 original words are verified first; `PLA_REALTIME=1` makes a mismatch fatal, the default-on path
  falls back to the game's own step (so another game version still boots at 30 fps).
- **Present pacing**: `present_ticks()` returns 1 instead of the game's 2, but only when
  `runtime_frame_step_active()` says the patch really landed — the two halves can never be separated by
  configuration, which is what protects against the 2×-speed failure mode from the docs.
- `PLA_REALTIME=0` restores the 30 fps semantics (no writes, no report line); `framestep <ms>` pins the step for
  diagnostics; `PLA_FRAME_STEP_REPORT_SEC` shortens the 30 s report.

Measured on this machine while it was starved by the desktop (≈16 fps regardless of anything the port does):
`[pla] real-time frame step: 533 frames in 29.6 s (18.01 fps), the game's steps add up to 29.53 s (0.9975 of the
frames' time); mean step 55.399 ms` — the game's own clock tracks wall time at every present rate, and
`@present_effective_ticks=1` while the game still asks for 2. The absolute 16.7 ms/frame was not observable here;
the same build measured 33.33 ms at 19:41 and 62 ms later with the identical binary, so the remaining number is the
desktop's, not the port's.

Launcher: the frame-rate selector now drives the native track (`60`/`120` leave `PLA_REALTIME` at its default,
`Original 30` sets `PLA_REALTIME=0`) and no longer requires the community 60 fps pchtxt — a game-side 60 fps patch
would write the same site and fight the port's.
