# Astral Chain port: work log

A record of how this port is being made, for anyone who continues it or tries the same with another game. Each entry
says what was found, what was done and why, what went wrong on the way, and how it was checked. Newest entries go at
the bottom of [Log](#log); [Status](#status) is kept current.

Conventions: dates are absolute; "verified" means a check with a recorded result, never "it looked right". Game data,
keys and saves are never committed (see `.gitignore`).

## Status

| Stage | Goal | State |
|---|---|---|
| 0 | Feasibility: does the game fit the by3 architecture? Code and data out of the dump, verified | done 2026-10-29 |
| 1 | Code rewritten for Apple silicon; merged RomFS | done 2026-10-29 |
| 2 | A runtime fork that boots to `nnMain` | done 2026-10-29 |
| 3 | Follow every unknown service / command / NVN call until the first `nvnQueuePresentTexture` | **done 2026-10-29**: first presented frame at 0.196 s (`nvnQueuePresentTexture` → the screen); the game then runs at its own 30 fps pacing — 10 800+ frames over 6 min verified |
| 4 | Playable: picture, audio (Wwise), input, saves; 30/60 fps and resolution patches | |
| 5 | TotK-grade polish: app, presets, shader seed, shared shader list, MetalFX | |

## Log

### 2026-10-29: stage 0, feasibility

Inputs: one 9.8 GB XCI (`异界锁链-v1.0.1-….xci`, card image) holding **base and update Program NCAs** —
base `6db3410b….nca` 10.34 GB (title `01007300020FA000`, code 1.0.0, cnmt version 0) and update `31735cd4….nca`
195.8 MB (base title ID retained, Meta `01007300020FA800`, cnmt version 65536 = **1.0.1**, the game's final version).
`prod.keys` from the same Switch. A community cheat file for 1.0.1 sits next to the dump; its folder name is the
build ID prefix it was made for, which is how the build ID below is cross-checked.

Findings, each read from the dump:

- **The update code is retail 1.0.1.** main's build ID is `4B159F0F7A360669738390EE8657B4F…`, matching the cheat
  folder prefix; the base NCA's main is 1.0.0 (`ED2026B20685971F8…`). The build uses the update's code, as with by3.
- **Two modules, not three**: the ExeFS is `main`, `sdk`, `rtld`, `main.npdm` — **no `subsdk0`** (by3 and TotK have
  three). The loader, Makefile and prepare scripts must handle a two-module set. main is 7.7 MB NSO,
  214.9 MB decompressed (by3's main is smaller); sdk is 5.2 MB NSO, 13.1 MB decompressed. Every ExeFS block hash
  verified for both.
- **43 services in the npdm** and no NPLN/gRPC/OpenSSL stack — this 2019 game predates it, so the online stubbing
  by3 needed (`nifm`, sockets, `ssl`, `friends`, `bcat` plus NPLN's own services) collapses to the plain offline set.
  Notable allows: `nvdrv`/`vi:u` (NVN), `audren:u`/`audout:u`/`audin:u` (audio), `hwopus` (Opus decode),
  `pl:u` (shared fonts), `ldr:ro`, `aoc:u` (DLC), `acc:u0`, `mii:u`, `irs`, `nfp:user`, `caps:su`/`caps:u`.
- **NVN surface ≈ by3's.** main imports 513 `nvn*` names; 512 are in by3's 523-name table, the one gap is
  `nvnBootstrapLoader` (older SDKs fetch the function table through it — needs one host entry in the loader/thunks).
  The middleware version string in sdk reads `1.2.11.f-NINTENDO-SDK-v1`; the SDK release number is not exposed as a
  plain string and will be read from `nvnDeviceGetInteger` at boot.
- **Middleware survey** (strings of decompressed main):
  - Audio is **Wwise** (`sound/WwiseInfo.wai`, `.bnk` ×33, `.wem` ×8172 in the RomFS, `AK::OpusDecodingThread`
    decoding through `nn::codec::HardwareOpusDecoder` / `hwopus`) — the same shape by3 already plays through
    `audout:u` with `runtime/audio_out.c`.
  - Data is **Platinum `.pkz` archives** (82 files, 7.58 GB) — by3's game lists `Contents:/` to find them;
    `movie/ev%04x.avi`-style strings exist but…
  - …the actual movies are **`.bk2` (Bink Video 2), 412 files, 1.85 GB** — the one genuinely new middleware.
    TotK's movie host decodes VP9 WebM; Bink 2 needs its own decoder. Plan: stub the movie host first (complete
    playback events without frames) so nothing hangs, real decoder as a later stage-4/5 item.
- **RomFS.** The base section verified through all six IVFC levels: 8,701 files, 10,309,560,892 bytes,
  SHA-256 `cdfb7043…`. The update NCA carries a 182.8 MB BKTR patch section (IVFC superblock, AES-CTR-Ex) whose
  indirect table points into the base section or the update data — by3's `merge_romfs.py` layers, minus the sparse
  layer (this base is complete, being a card image). Stage 1 adapts it: base `romfs.bin` is already decrypted and
  verified, so the merge reads the update's indirect + AesCtrEx tables and applies them over the base image, then
  verifies the merged image through the update section's own six IVFC levels.

Tooling brought into this folder (see `third_party/COPIES.md`): `extract_exefs.py` (by3) and `extract_romfs.py`
(totk), both changed to take `--title` / `--update-title` instead of hardcoded Tears of the Kingdom IDs; with a card
image holding both Program NCAs, `program_nca` now picks the update NCA by its BKTR section marker and accepts the
update's Meta NCA in the base branch. Pitfall found on the way: by3's `extract_exefs.py` was an un-adapted TotK copy
(by3's own tool is `extract_code.py`); the TotK title-ID error is how this surfaced.

Verified:

- ExeFS (update, and base for the record): FS header, hash table and every block verified (`extraction-manifest.json`
  in `AC_build/extracted/v10{0,1}/`).
- Base RomFS: all six IVFC levels, 629,246 data blocks at level 5; one flipped byte would fail a block.
- `assets/expected-v1.0.1.json` pins: title/update title IDs, cnmt versions, main build ID, both Program NCA
  identities, the update ExeFS hashes, and the base RomFS size/SHA-256/file count. The merged-RomFS and
  rewritten-module hashes are stage 1's additions.

Open questions carried forward:

- SDK/NVN version numbers (read at boot; shader GPU-code version expected ≤ the layer's table range, older is safer).
- npdm's application pool and heap figures (by3 learned its pool at `svcGetInfo`; same expected here) — stage 2.
- Whether `nvnBootstrapLoader` needs a real implementation or the game's nvdrv client never calls it when all names
  resolve — stage 2/3.
- Bink 2 strategy: stub first, decoder later (stage 4).
- `ldr:ro`, `htc`/`htcs`, `banana`/`o6j`/`pcm`/`s2g`/`ntc`/`bsdcfg` in the service list: probably allowed but
  unused; the host fails loudly if one is really called.

### 2026-10-29: stage 1, the code rewritten for Apple silicon; the merged RomFS

**The patch area had to move below the module base.** main decompresses to 214.9 MB, 191 MB of them zero-filled
BSS (the engine's static arenas; the guest reaches them through adrp pairs the rewrite does not touch, so BSS
cannot move). The rewriter appends the patch area after the image, which put it ~200 MB from the first text
sites — beyond a B/BL's ±128 MB reach. sdk was never a problem (13 MB image). The d2r copy in
`third_party/d2r` now writes the patch to its own file `<name>.patcharea.bin` and records a signed `PATCH_OFF`
plus `PATCH_SIZE`/`ARENA` in `fixups.tsv`: images over 96 MB take a 32 MB arena below the module base
(PATCH_OFF −0x2000000, every site within ~42 MB), small images keep the patch appended exactly as before
(sdk's flat view is byte-for-byte the old one-file layout). The runtime loader maps the two parts at their
true addresses — stage 2's `map_module` change.

**Rewrite results** (`DM_RULESET=native DM_PRIVATE_GATES=0`): sdk 10,957 sites (patch 440,628 bytes), main
5,426 sites (patch 219,824 bytes). main needed two rewriter extensions:

- `ldrsw_table` (the BotW-1.6.0 jump-table shape) did not match Astral's variant at `main+0x6db4a0`: the
  `ldrsw` writes the index register itself (E == I, perfectly fine — the index is dead after the load), and
  the table base (`adrp x24, #0xbec000; add x24, x24, #0x944`) sits at the function's entry
  (`0x6dad60`), ~1,800 instructions from the dispatch, not in the 48-instruction window. The scanner now
  accepts both: after the load only the table base must survive, and the base pair is searched from the
  function start. Five such tables matched; every entry target is checked to be an instruction of its
  function, as before. (Table at `0xbec944`, 6 entries, targets `0x6db4a4`–`0x6db544`.)
- One `spset` site (`mov sp, x18` — the by3-era block kind) — the earlier extension paying off.

**Verification** (all recorded):

- `structck.py`: sdk 0 violations, 11,106 trampoline exits all legal; main 0 violations, 5,543 exits all legal.
- `equiv.py` (Unicorn, random states, per site ≥2 effective trials): sdk 29,508 effective trials, 0 divergent;
  main 14,531 effective trials, 0 divergent. Negative controls: 600 mutations per module (half a trampoline
  exit retargeted +4, half the replaced instruction nopped) — all 1,200 caught.
- Two rewrites give identical bytes (`build_src.sha256` pins the rewriter).

Pitfalls of this stage, so nobody repeats them (all in the adapted checkers, each caught by a failing run):
a checker that walks the patch must convert decoded branch targets back to guest addresses (PC-relative
targets inherit the flat view's shift); patch-region membership needs the upper bound once the patch is not
the only thing above the text; capstone reports an instruction at a negative base as an unsigned 64-bit
address; and "is this branch back to text" must ask "outside the guest patch span", not "below PATCH_OFF" —
that one is inverted for the arena layout.

**Merged RomFS.** `scripts/merge_romfs.py` (adapted from by3's merger: no sparse layer — this base is
complete) applies the update's BKTR patch section (indirect table + one AES-CTR-Ex row) over the verified
base `romfs.bin` and verifies the merged image through the update section's own six IVFC levels:
629,288 blocks at level 5, 8,701 files, 10,310,254,332 bytes, SHA-256 `20904f30…`; 10,147,756,960 bytes from
the base, 182,747,996 from the update. One real find on the way: the indirect table's storage-0 targets are
base **section** virtual offsets — the base section begins with its own hash tree, so reads must subtract the
base section's level-5 offset (`0x134c000`). The first attempt (reading romfs.bin directly at the target)
failed IVFC level 5 block 9; level 4 had verified, which is what localized the defect to the base segment.

`prepare_modules.py` (by3's analysis-ELF tool, `subsdk0` skipped) inventories the two modules: main exports
24,737 symbols / imports 13; sdk and rtld recorded in `AC_build/extracted/v101/elf/`.

`assets/expected-v1.0.1.json` now pins the rewritten modules (`main.patched.bin`, both `patcharea.bin`s,
`sdk.patched.bin`) and the merged RomFS alongside the stage-0 records. The build re-checks every one of them.

### 2026-10-29: stage 2, a runtime fork that boots to `nnMain`

`games/alc/runtime` is a fork of `games/by3/runtime` (repo commit `6b0c377` is the unchanged copy; the diff
since is this port), with its own copies of the graphics layer (`third_party/d2r_gfx`, compiled unchanged —
it carries the Platinum-relevant fixes: GPU-side indirect draws, per-stage program binding, attachment-less
passes, `preserveInvariance`, `X32_Stencil8`), the prebuilt shader translator, and the by3 scripts. The NVN
layer reports 523 names, 424 implemented; Astral's 513 imports cover all but `nvnBootstrapLoader`, which the
layer answers as an unimplemented stub until the game calls it (it fails loudly with its name).

**The loader learned the two-file patch layout** (stage 1's arena model): the module table carries a signed
`patch_off` plus `patch_size` and the patch area's SHA-256; `map_module` allocates `[base−arena, base+size)`
as one region (`mmap` anonymous), reads `patched.bin` at `base` and `patcharea.bin` at `base+patch_off`,
verifies both digests, and `seal` makes the patch area executable and flushes its icache. The permission
self-check treats the patch as the guest span `[patch_off, patch_off+patch_size)` — with the patch below the
base, a negative `patch_off` never matches a guest offset. The runtime tables bind everything: 87,192
relocations, main's `GetSystemTick` PLT found uniquely at `main+0xa0d630`, both modules' `DT_INIT`
constructor-state words detected by the same adrp+ldr shape TotK and by3 use (compiler pattern holds across
the older SDK).

**One SDK difference surfaced at boot:** Astral Chain's SDK exports a four-callback `nn::init::Start`
(`_ZN2nn4init5StartEmmPFvvES2_`, no finalizer) where TotK/by3's SDK 13+ passes three; the boot tries the
four-argument form first and falls back. Everything else in the host answered as-is.

Verified (recorded):

- `alc-native rewritten/v101 probe`: pass. Both modules mapped (main `size=214892544 patch=−33554432+219824`,
  sdk `patch=13107200+440628`), 87,192 relocations applied (main 55,255 relative + 624 symbolic;
  sdk 9,048 + 22,265), identity checked, 4,000 cross-module PLT calls main→sdk→host timer on 4 threads.
- `TOTK_HEADLESS=1 TOTK_VERIFY_STARTUP=1 alc-native rewritten/v101 boot`: pass (phase 4). The original
  `nn::init::Start` ran the SDK startup; the merged RomFS verified; every constructor finished with state 2
  (main 145 init_array entries, sdk 105); the original `nnMain` ran to its focus-handling setup, called from
  `main+0x9c284c`. This is the first Astral Chain code running natively on the Mac.

Pitfalls of this stage: with the patch in its own file, `patched.bin` must still be padded past
`KEY_PAGE_OFF+0x4000` — the appended-area layout used to cover the key page's tail by accident, and an
un-padded file fails the loader's key-page bound (main's image is not 16 KiB aligned); and a signed
`patch_off` must never be widened through `size_t` before the sign is handled (`(size_t)(−0x2000000)` put
the map size at 2^64−3.2 GB, which `mmap` "succeeded" at and the first `fread` reported `EFAULT`).

Open questions carried forward: `nvnBootstrapLoader` (unimplemented stub; abort names it when reached); the
application pool figure (the host's default stands until the game's `svcGetInfo` asks); Wwise's audio
renderer revision under the older SDK; Bink 2 movies (stage 4).

### 2026-10-29: stage 3 (in progress), from `nnMain` into the game's own init

Method: as by3's — boot with a window, read where the host stops or the guest crashes, fix that, repeat.
`scripts/symbolize.py` (by3's, work folder renamed) names sdk frames from the analysis layout.

Answered so far:

- **nn::vi answered by name** (`runtime/system_services.c`, the `runtime_alc_override` table): the host has no
  vi IPC service and presentation is the host's own window (by3's model). `Initialize`/`SetLayerScalingMode`
  return 0, `OpenDefaultDisplay`/`CreateLayer` hand back distinct non-null markers (the game does not check
  the results and dereferenced a null layer at `nn::vi::SetLayerScalingMode` — the crash that motivated this),
  `GetNativeWindow` returns no native handle, which the graphics layer tolerates.

State: the window is created and focused, the Metal device comes up (Apple M5), the native VI display
initializes, the game lists `Contents:/` (47 entries; `miscTex`, `movie`, 389 `mv` entries, `sound/stream`
with ~7,600 entries) and spawns its worker threads; `nnMain` proceeds into its own initialization and
crashes at `main+0x9c0d1c` (`ldr x24,[x10,#8]` with `x10 = NULL`).

The crash is traced to its data flow: `adrp x23,0xdc2000; ldr x23,[x23,#0x690]` loads the pointer at
`0xdc2690` — a RELR relocation (addend `0xcc9cd98`) points it at a BSS object — and the object's field at
`+0x28` is NULL when the game reads it (`x10 = x23->field_28`, then `ldr x24,[x10,#8]`). An
effective-address scan over all of main's text finds **no store to that field**: the BSS object
(`0xcc9cd98`) is populated by an init step that has not run yet (or failed silently — the `-14` in x4 is
a candidate return value to chase). The caller chain is `nnMain → main+0x9c26b8 → main+0x3e3b90
(bl main+0x9c1bd0)`.

Next step: find the writer of the BSS object at `0xcc9cd98`. Static scans are exhausted: no store to the
object anywhere in main's text (effective-address scan including adrp+add chains), no init_array entry
references it, and no call passes it or its pointer field in x0–x7. The field must be written through the
relocated pointer at `0xdc2690` loaded from memory (or by SDK code), so the writer hunt moves to lldb:
`scripts/crash_probe.py` is an lldb Python command (`lldb -b build/alc-native -o "command script import
../scripts/crash_probe.py" -o alcprobe -o quit`, from `AC_build`) that waits for main's `seal`, reads the
module base dynamically, arms a write watchpoint on the field, breaks once at the crash function's entry
and dumps guest registers, the object and the pointer table at the SIGSEGV. State of the script: it
reaches the base read (`alcprobe: main base = 0x10a65c000`) and then returns before arming — the next
session finishes that function (one API round-trip) and reads the watch. `scripts/pltres.py` resolves
main's PLT targets to import names (used to identify init steps). Note: the module base is NOT stable
across lldb runs (0x10a638000 vs 0x10a620000 vs 0x10a65c000 observed) — always read it dynamically. The
registry reads a mutex at the neighbouring pointer field `0xdc2688` and the crash walk pattern appears in
a second place (`main+0x9c1bf0`), i.e. this is a C++ listener registry whose sentinel is missing.

This is the shape of the whole stage: by3's log records the same loop until `nvnQueuePresentTexture`.

Pitfalls: none new this round; the instrumentation added to `loader.c` during the boot fixes (read/short,
key page, permission mismatch) is temporary and off by default in behaviour.

### 2026-10-29: stage 3 session 2, the writer hunt is armed

`scripts/crash_probe.py` now runs end to end up to the arm: it launches under lldb, waits for main's
`seal`, reads the module base dynamically (0x10a648000 this run — confirmed unstable across runs), and
arms an 8-byte write watchpoint on the crash field (`base+0xcc9cd98+0x28 = 0x117340dc0`,
`WatchAddress(field, 8, False, True, err)` — this lldb takes booleans, not the enum constants, for
read/write) and reports `valid=True err=success`. The session then ended before the stops were reported:
`dump()` needs the same try/except + flush treatment the arm step got (an unhandled exception in a batch
script ends the whole session silently). Next session: run it again, read `[watch#…]` / `[SEGV]` blocks,
and the writer question answers itself.

The same session also confirmed, independently of the earlier static scans, that the crash reproduces
identically under lldb (`EXC_BAD_ACCESS code=1 address=0x8` at `ldr x24,[x10,#8]`, thread #7).

### 2026-10-29: stage 3 session 3, the walker SEGV was a symptom; NVN ABI and `hwopus` fixed

**The watched field is never written by anyone.** `crash_probe.py` (reworked: the registry object is read
dynamically through the relocated pointer at `base+0xdc2690` — the earlier session's `0xcc9cd98` addend
reading was a misread; the `.rela.dyn` addend is `0xccb6298`, which the loader had applied correctly) armed
write watchpoints on the object's first field and on field `+0x28` and both stayed silent from `seal` to the
SIGSEGV, three boots in a row. Static scans agree: no instruction in main's text stores to those fields
through any path (53 loads of `[0xdc2690]`, 74 loads of the neighbouring slot table, adrp+add materializations
— checked with a register-taint walk over `/tmp/main.dis`-style full disassembly).

So the hunt changed shape: find what *should* have constructed the registry, and why it never ran. A chain of
lldb probes (all in `scripts/`, one guarded round-loop each — this lldb also ignores `SetCondition` on
one-shot breakpoints, so every hit is logged and filtered in Python; an unguarded script dies silently at the
first exception and takes the batch with it):

- `publish_probe.py` proved the registry's publisher (`nnMain` at `main+0x3e3794` → `0x9c1a80` init of a stack
  temporary → `0x9c1ab0` publish → `0x9c0a10` insert) never ran: the big init that contains it bails at the
  guarded call `main+0x3e3748` → `bl 0x40d5a0`, and the `0x3e3440` wrapper then falls *unconditionally* into
  the `0x3e3af0` chain whose `bl 0x9c1bd0` veneer is the walker — the SEGV's exact shape (by3's stage-3
  lesson, learned again: a null dereference in a later path is a symptom of an earlier failed init).
- `fail_probe.py` walked `0x40d5a0`'s ~50 guarded calls: the first one fails (`bl 0x40dfd0`, the NVN window
  and shader-system init, args 1600×900).
- `trace_probe.py` gave the whole flow in one boot: `0x40dfd0` builds the descriptor, `0x40f8e0` builds the
  swap-chain list successfully, then `bl 0x9dea90` (NVN device init through the table at `[0xdc2b48]`) fails.
- `cond_probe.py` walked `0x9da600`'s checks and then `0x9da920`'s: descriptor checks pass, both allocations
  succeed, the three `nn::vi` calls return success (`OpenDefaultDisplay`/`CreateLayer` write the `0x11`/`0x21`
  markers into the out parameters and return 0 — the overrides were correct), and the failure is `bl 0x9f2780`.
- Inside `0x9f2780`: `nvnBootstrapLoader("nvnDeviceGetProcAddress")` works, the driver table resolves, and the
  failure is the version gate at `main+0x9f2850`: `nvnDeviceGetInteger(0)` must equal `0x35` (53) and
  `(1)` must be ≥ `0x136` (310). The layer answered TotK's 55/15.

**Fix: GAME_ALC.** The layer's device-info table now has a `GAME_ALC` branch with `53, 311` (311 ≥ 310; the
Tegra X1 pairing measured on SMO/BotW), `-DGAME_ALC` in the Makefile's `GFX_FLAGS`. BotW's separate
`rt_dup_slot` rendering workaround in `nvn_obj.h` stays out of this build.

**Verified:** device init passes (`WindowInitialize`, swap-chain 3×1600×900), the whole `nnMain` init chain
runs to its end — registry published, walker passes, audio init starts — and the game's main loop is alive
(it reads input and sends a vibration value). The earlier `hwopus` wall is next.

**`hwopus` (the Switch's hardware Opus decode).** Wwise's stream decode goes through
`nn::codec::HardwareOpusDecoderManager`: `GetHardwareOpusDecoderWorkBufferSize(rate, channels)` connects to
`hwopus` per rate (the size queries are cached in a static table). The by3-era `nn::codec` overrides cannot
intercept here — main imports only the `HardwareOpusDecoder` constructor, and the manager path runs inside
the sdk module behind a virtual call. So the service itself is implemented (`runtime/hwopus.c`, libopus
already linked): command 1 is the work-buffer size query (raw `{rate u32, channels u32}` → `u32`), answered
for all five rates; command 0 (open, work buffer as a move handle) is implemented per the client's argument
shape and will be exercised once the boot gets further; unknown commands dump their raw payload and stop the
runtime loudly.

**Where it stands:** the boot parks with the main thread spinning at `main+0x3e415c` (`ldr w9, [x8, #0x194];
cbnz w9, self` — nnMain region, entered from `0x3e4028` under state bits 28/29 of a status word, i.e. a
conditional "wait for in-flight operation" path around the `[0xdb84c8]` state object) while every guest
worker thread waits on a host core. Nobody writes `+0x194` through the object pointer within any scan
window; the atomic clear at `0x3e41b0`/`0x3e4200` has no direct caller (a callback via the `0x3e4240`
descriptor into `0x9deb90`). Next session: find the setter of `[0xdb84c8]+0x194` and what it waits for
(live-attach lldb on the parked process worked well for this: read `modules[0].base`, break at the spin, sample).

### 2026-10-29: stage 3 session 4, the park is an nn::os fiber that never resumes

With `hwopus` answered, the boot gets through audio init and the game's main loop starts (input and
vibration live), then parks. Live diagnosis (sample + lldb attach on the parked process; module bases are
unstable across runs and two early frame attributions in this log were hex-conversion errors — always read
`modules[0/1].base` from the probe and check every address against the module range):

- The main thread spins at `main+0x3e415c` (`ldr w9, [x8, #0x194]; cbnz w9, self`), entered from
  `main+0x3e4028` under state bits 28/29, waiting for `[0xdb84c8]+0x194` (a state object at
  `main+0xdcbe30`, pointer-relocated at `0xdb84c8` — the only alias) to clear. At the park it holds 1, and
  a 15 s write watch sees nobody touch it (`live_flag_probe.py`).
- Every guest worker thread is parked in a normal guest wait. Nothing is deadlocked in the runtime's
  virtual-core scheduler: the spin simply never yields its core, and the rest wait for events that never
  come.
- The field is the **async-phase flag of an `nn::os` fiber job system**. The chain, from a corrected
  backtrace (`fiber_probe.py`, one headless boot, breakpoints on the whole path): the manager at
  `[0xdcc2c0]` gets the pair `(waiter 0x3e4120, completion 0x3e41b0)` registered via
  `0x9c6dd0/0x9c6e50` (the scheduler module around `0x9c5exx/0x9c6exx` — `nn::os` semaphores and
  `GetCurrentFiber` wrappers), the job starts (`InitializeFiber` at `main+0x9c60b4`), the fiber is switched
  to twice (`SwitchToFiber` → sdk's `FiberImplByHorizon` context switch at `sdk+0x5125c0`,
  `CallFiberFunction` fires — the fiber body *starts*), and then nothing: no further switch, the phase job
  (`0x3e4240`: `0x3c1500` → `0x40d1b0` — the shader-system init — → the registry remove-walk veneer
  `0x9c1be0` → `0x9e3e80` → `0x6e5310`) never executes, the completion callback never runs, the flag stays
  1, and `nnMain`'s wait spins forever.
- The exact broken link is open: after the second `SwitchToFiber` (the yield at `main+0x9c5b74`) the pump
  should be resumed with the phase job, and the resume never happens. Next session: find who is supposed to
  hand the job back (a worker thread's semaphore wake that our host never signals, or a second fiber that
  was never started), and check the first `CallFiberFunction` x0 — it read as `main+0xa0e4f0`, a PLT stub
  (`AcquireReadLock`), which is either an argument-register mis-read or the real trampoline.

Probes added this session: `spin_probe.py` (flag watch, headless), `live_flag_probe.py` (attach to the
parked process, read + watch the flag), `encode_probe.py` (sdk encoder backtrace; the "Opus encode stall"
it once showed was an artifact of the flag watchpoint's own overhead — the field is written often during
normal init), `fiber_probe.py` (the ordered fiber/phase trace).

### 2026-10-29: stage 3 done, the first frame is presented — and the game keeps running

**The fiber's waker: there is none to find — the scheduler starved it.** `fiber_probe.py` had shown the
task fiber created (`InitializeFiber`), switched to twice, `CallFiberFunction` fired, and then nothing. The
answer was not a missing caller: the task manager **waits for its fiber with a user-space spin**
(`main+0x3e415c`, `ldr w9, [x8, #0x194]; cbnz w9, self` — no SVC anywhere in the loop), and the runtime's
virtual-core model can only preempt at SVC entry (`runtime_core_checkpoint` inside `runtime_svc`). The
spinning thread never yields its core, the fiber's worker never gets one, and the phase job never runs.
The hardware has three real cores that run the spin and the workers simultaneously; the model has to be
told to match it: **`TOTK_FREE_CORES=1`**. With free cores the fiber resumed immediately, the phase
completed, and the boot sailed past the park.

Two more walls fell the same hour:

- **`TOTK_GPU_MIDCLEAR=1`**: Astral clears colours mid-pass, masked and scissored on its normal boot path;
  the TotK-era isolation guard aborted the layer's first `QueueSubmitCommands`. Enabled, the clear runs.
- **`nn::account::OpenPreselectedUser`** (the guest asserted → `nn::diag` Break right after "persistent
  local account initialized"): the game calls the non-Try variant, which is not in `runtime/account.c`'s
  override table, so it reached the sdk-native path, read its own empty preselected-user cache and returned
  a failure the game treats as fatal. The override now answers from the same persistent local identity as
  `TryOpenPreselectedUser` (an `nn::Result` instead of a bool).

**Verified: first presented frame at 0.196 s** (`[totk-boot] 0.196 first presented frame`;
by3's was 0.64 s), then the game keeps running at its own 30 fps pacing — frame 6000 at ~3.5 min, frame
10 800 past 6 min, present intervals p50 33.33 ms on the 120 Hz display, GPU 94% busy, all of it through
the runtime shader translation service (compute shaders included). The launcher (`make boot`) now sets the
three switches with the reasons in a comment: `TOTK_FREE_CORES=1`, `TOTK_GPU_ALLOW_SUBMIT=1` (the TotK-era
kernel-panic guard; Astral submits from frame 1 on this machine), `TOTK_GPU_MIDCLEAR=1`.

Also this session: `make shader-tools` was never run in this tree, so the first shader translation had
started the .NET fallback service, which needs a `scripts/shader_service.py` this fork never had; with the
prebuilt `totk_xlate` in place (`make shader-tools`, SHA-256 checked) the runtime service works. The stage
goal — from `nnMain` to the first `nvnQueuePresentTexture` — is met; stage 4 (playable: audio heard, input,
saves; the 30/60 fps and resolution patches) starts from a game that already draws, drives its own frame
pacing, and talks to the audio services.

### 2026-10-29: stage 4 begins — the character-select crash was the missing pl:u shared-font service

The user played the live window through the title into the character select and the game died there —
exit code 1, and the log's tail held the answer: `sm: service "pl:u" not implemented`, callers
`sdk+0x32a3d4` = `nn::pl::RequestSharedFontLoad` then `nn::pl::GetSharedFont` (the character-select UI is
where the game first loads the system shared fonts to draw the names). The older SDK drives `pl:u` over
IPC; the name overrides in `shared_font.c` cover only the three non-IPC wrappers the boot path never used.

What went in:

- The six system fonts (standard, Chinese simplified + extension, Chinese traditional, Korean, Nintendo
  extension) copied from this machine's own by3 extraction into
  `AC_build/extracted/firmware/shared-fonts` and `make prepare` regenerating the verified rows — they had
  never been extracted in this tree (the config was six empty rows).
- `runtime/pl_u.c`: the `pl:u` service — RequestLoad, GetLoadState (1 = Loaded, the SDK's own enum name),
  GetSize, Load into a client buffer, and GetSharedFont (command 6) answering the SharedFontData array
  with the `shared_font.c` region's real pointers: the runtime maps guest memory in the same address space
  as the host, so no shared-memory mapping is needed. Anything else dumps and stops loudly.

Verified en route: saves write for real — the user's playthrough produced `GameData.dat` and
`SlotData_0.dat` (~15 MB) under `user-data/save/`; input drives the whole title flow on keyboard
(WASD left stick, arrows right stick, J/K/U/I = A/B/X/Y, Q/E = L/R, Option/Shift = ZL/ZR, Return = +,
Tab/Esc = −, F/T/H/G = d-pad); Core Audio output is live (`AudioOut open 48000 Hz x2 PCM16 → Core Audio
(stereo)`, the AQ converter and IO threads active).

The game was left running for the user to reach the character select again — that leg verifies the
service. Also fixed en route: the runtime shader translation had silently fallen back to a .NET script
this fork never had; `make shader-tools` (the prebuilt, SHA-256-checked `totk_xlate` + SPIRV-Cross) puts
the real service in place.

### 2026-10-29: stage 4, verified past the character select — the prologue runs

Played on a Mac (v1.0.1): the character select renders the names (the pl:u/GetSharedFont fix holds), a
character is picked, and the prologue runs normally from there; ~24 700 frames presented over the session at
the game's own 30 fps pacing, audio out through Core Audio, and the session ended through the window's
graceful-shutdown path — that path is verified too. The frontier moves to the first battle, the cutscenes
(Bink2) and the rest of stage 4: audio audibility, the 30/60 fps and resolution patches, the app.
