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
| 4 | Playable: picture, audio (Wwise), input, saves; 30/60 fps and resolution patches | **done 2026-10-30**: playable at 60 fps (14-site patch) with a 1920x1080 internal resolution (9-site patch), tested across several chapters by the player; settings panel, keyboard/mouse/controller remapping and saves verified; frame interpolation lands here but is still under development at 30 fps modes |
| 5 | TotK-grade polish: app, presets, shader seed, shared shader list, MetalFX | **in progress**: self-contained app with presets, shader seed and the TotK-style straight-in launch done; shared shader list contribution and the remaining polish open |

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

### 2026-10-30: stage 4, the 60 FPS patch — the game's own frame-rate mode 0, switched on

No 60 fps patch exists to borrow: every patch file under the user's Switch titles tree (4 511 pchtxt/ips/txt files)
was searched and Astral Chain's are money/HP/speed cheats only. The reference instead came from this repo's TotK
60 FPS work (the shape of a checked site table) and from the mechanism the community's Astral Chain dynamic-fps
project documents publicly — ideas only; every site below was located and verified in this game's own image
(decompressed main 1.0.1, cross-checked against the rewritten image byte-for-byte before patching).

**What the game has natively.** A frame-rate mode n ∈ {0 = 60 fps, 1 = 30 fps}, consumed in three places:

- the window interval: `n == 1 ? 2 : 1` feeds `nvnWindowBuilderSetPresentInterval` (start-up, main+0x9e8d84) and
  `nvnWindowSetPresentInterval` (run-time, main+0x9db6bc) — the run-time call never fired in any boot because the
  main loop (main+0x3e39d0) requests mode 1 every frame and the interval never changes;
- per-mode timing conversions (main+0x9a0c58: divide by 30.0 at mode 1, by the 60.0f at 0xb5d47c at mode 0);
- the logic step: once per frame the timer (main+0x91dbb0) computes `step = clamp(measured_ms × 0.03 − carry,
  1.0, 2.0)` in 30 fps frame units. The 1.0 floor — `fmov #1.0` at main+0x91dc94/0x91dca0 and the 0x3f800000
  store at main+0x91dcac — is the whole reason an interval-1 game would run 2× fast: at 60 fps a frame measures
  ~16.7 ms = 0.5 steps and the floor rounds every frame up to a full 30 fps step.

**The patch** (mods/1.0.1/60fps/exefs/60fps.pchtxt, generated by scripts/make_fps60_patch.py, 14 rows): the main
loop requests mode 0 (main+0x3e39d0 `mov w0,#1`→`#0`); both interval sites forced to 1 (`csinc`→`mov w1,#1`);
the step floor 1.0→0.5 (three instructions). Two consumers truncate the step to whole 30 fps ticks (the 30-tick
trigger at main+0x48be14 and the tick-parity branch at main+0x57e7e0) and would freeze at a fractional step, so a
13-instruction cave in the all-zero assembler padding between .text and .rodata (main+0xa0e7d0, mapped r-x with
the text) accumulates the fractional step and publishes whole ticks at timer+0xd8 (timer+0xd4 holds the fractional
carry — both fields are free: the per-frame flush zeroes them, and the deficit carry that otherwise owns +0xd4 is
inert once the floor is 0.5, since a 60 Hz present can never measure under 16.7 ms); the carry-in add
(main+0x91dc14/18) is nopped so the published ticks cannot leak into the next frame's measured-time sum, and the
consumers read timer+0xd8 directly. The timer object is at main+0xdcbfd0 (live-confirmed).

Mechanics on the way: the game's pchtxt parser only treats `//` as a comment, so a `#`-commented pchtxt row is
silently dropped (the cave row initially vanished; prepare_patches prints the row count — read it). The
verification script originally held six bare `SystemExit(...)` without `raise` — every check ran, matched nothing,
and passed nothing; its first real run caught a mis-encoded `fsub s0, s0, s16` in the cave. game_patches.c's
"rewrite area" guard learned to accept rows that lie entirely inside the text padding; scripts/prepare_patches.py
moved from by3's paths to this game's (expected-v1.0.1.json, v101, AC_build, mods/1.0.1).

**Verified**: with `ALC_PATCHES=60fps` the patch applies (14 sites, originals re-checked at load), the window
builds with interval 1, and the game presents at p50/p95/max 16.67 ms on the 120 Hz display — 8 100+ frames over
2¼ min, 0 system drops (the 30 fps baseline measures p50 33.33 ms) — with the first presented frame at 0.237 s.
A live probe (scripts/fps60_live_probe.py, lldb attach) reads the timer mid-run: step 0.501, fractional carry
0.501 — the logic advances half a 30 fps step per frame, i.e. correct speed, not 2×. The game was left at the
title screen; nobody has played it yet. **Open**: interactive play at 60 fps (prologue, first battle), cutscene
speed (Bink2 — the step's override path, untested in this port regardless of frame rate), and the launcher preset
flow (`fps ≥ 60` already selects the `60fps` patch slug). Run it with `ALC_PATCHES=60fps make boot`.

### 2026-10-30: stage 4/5, MetalFX frame interpolation on Astral Chain — the TotK pipeline taught this game's frame

The shared graphics layer carried the whole MetalFX frame-interpolation pipeline (totk_mfx.m: motion vectors from the
G-buffer depth + a camera block, the FrameInterpolator, per-mode present pacing) — but every input hook was TotK-shaped
and never fired for Astral Chain ("缺输入 画面/相机/深度/UI" on every frame). Four adapters later it runs end to end:

- **Depth**: Astral Chain has no ≥3-colour G-buffer; it renders a depth-only pre-pass (nc==0) and single-colour lighting
  passes testing LEQUAL against it. The pre-pass depth (D24S8, which Apple GPUs run as Depth32Float_Stencil8) is fed to
  the interpolator as-is — the FI descriptor now follows the actual format. A D32F *view* of D32S8 turned out to be not
  viewable on this GPU (Metal validation aborts in newTextureViewWithPixelFormat, taking the process down), and
  tex_upgrade_pfv does not help; the raw texture works because depth sampling is the same operation.
- **Scene**: TotK ends the scene at the first UI-shader draw into its canvas. Astral Chain composites the float canvas
  into the presented texture and draws UI on top of it there, so the boundary is the first draw into a presented
  texture; the snapshot then copies the learned canvas (serial_get) instead of the pass's target. That composite draw
  itself must not enter the UI layer (it IS the scene) — a one-shot role skip handles it.
- **UI layer**: attachment 7 rides on the passes into the presented texture (after the snapshot), same premultiplied
  PSO variants as TotK (fs_fn_mfx). Gate: only passes whose target is the presented texture **at the canvas
  resolution**. The game also presents boot/intro movies at their own lower resolution directly, and an 800x450 UI
  layer meant the mfx textures were recreated on every movie/menu switch — that churn took the GPU down
  (kIOGPUCommandBufferCallbackErrorInnocentVictim from frame ~15 on, 6 log-MB of victims). Two guards: canvas_learn only
  accepts a canvas near the presented size (≥90%), and a present whose size differs from the canvas aborts the
  interpolation for that frame instead of showing a 1600x900 interpolated picture in place of an 800x450 movie frame.
- **Camera**: the interpolation wants TotK's 84-float camera block; Astral Chain's depth pre-pass binds two row-major
  4x4 in vertex uniform 0 (view, then projection; the view translation moves with the camera) and
  near/far/tan(fovX/2)/tan(fovY/2) in uniform 13 (0.1, 9999.9, 0.828991, 0.466308 at boot). mfx_take_camera composes
  the block: view rows, VP = P·V, the translation as position, aspect = tanX/tanY, fov = 2·atan(tanY).
  ALC_MFX_UBODUMP=1 prints the binds once (the hunt that found this); TOTK_MFX_CAMLOG logs captures.

**Pacing**: the present clock is fixed at 60 Hz (DM_PRESENT_HZ) while the display runs 120 Hz, and "half the refresh
rate" was measured in present-clock ticks — it now means two display refreshes (the display rate is read live in
present_period), and without the 60 FPS patch (game interval 2) half-rate falls back to 30+30, so the 1.0 step floor
can never double the speed. The production-policy probe checks both shapes.

**Verified** (live, 60 FPS patch + TOTK_MFX=interpmax + ALC_MFX_UI): the canvas is learned once (1600x900); 12 300+
game frames with **99.3% of frames interpolated**, GPU cost 2.4–2.9 ms per interpolation; the present rhythm shows
**600/600 interpolated pictures at p50/p95/max 16.67 ms on the 120 Hz display** (60 fps game + 60 interpolated);
mode switches behave (30+30: 450/600 interpolated at 30 fps game; half-rate: 300/600 at 60 fps game; camera cuts
reset history and skip, no stuck frames). Gameplay, HUD and the load-menu screens all render normally with the UI
layer on; full screen (drawable 3024x1898) works — the MetalFX output upscaler path follows the window's drawable
size (it engages when the drawable exceeds 1600x900; the transition itself paused the present for ~80 s once and the
game set its interval 1 → 2 around the switch — rechecked with a real user's fullscreen pass, not scripted AX).

**Launcher**: the settings window gains an "Interpolated frames" control (Off / 30+30 / Half the rate / Every frame,
TOTK_MFX=interp30|interp|interpmax; off unless chosen, and ALC_MFX_UI follows the interpolation unless set). The
Quality preset becomes "120 fps, full screen" (60 fps game, every frame interpolated); Original keeps 30+30.

On the way: `pkill -f "^/path"` missed the running game (it was started as `./build/alc-native`, so the anchor never
matched) and the single-instance slot made the next launch wait forever with its input script firing into the void —
kill by pgrep pattern without the anchor; and grep on the active log file under-counts badly (a python line scan found
33 232 pass lines where grep -c reported 0), so log analysis reads the file in python.

**Open**: wshot (window screenshot) does not land while interpolation is on (to be looked at); the 30+30/half-rate
modes on a 60 Hz display are TotK-verified paths but were not re-checked for this game; long-session soak with
interpolation on the Prologue's heavier scenes.

### 2026-10-30: the interpolated frames ran one gamma too bright — and key 3 did nothing (both fixed, user-reported)

**White/washed-out picture with interpolation on.** The player saw the whole picture go bright whenever interpolation
was active. Measured on identical title-screen frames: the game frame's sky reads RGB 20,28,80 while the interpolated
one read 84,94,152 — and 113,133,176 against 54,76,124 — exactly the sRGB encode of the game's values (decode of 0.078
is 0.30; observed 0.33). Mechanism: Astral Chain's canvas already holds display-encoded values (the game's composite
blends them into the presented texture with plain linear mixing — checked the translated shader), but the shared
interpolator is built for TotK's canvas, which holds linear values: it interpolates them and writes an sRGB output
texture, whose hardware encode is what makes TotK's present come out right. Feeding it display values encodes a second
time and every interpolated frame arrived one gamma bright. Fix (this fork only): the frame interpolator's output
texture is plain RGBA8Unorm and mfx_output returns it directly — the round trip is identity. Verified: interpolated
sky 24,29,81 against the game frame's 20,28,80 (interpolation between two slightly different frames), the mid-right
region 59,72,124 against 59,73,125.

**Key 3 (output mode cycle) was a stub.** runtime.h declares key 3 as TotK's "MetalFX 2K -> native 2K -> off" cycle;
this fork's game_patches.c answered `return 0`, so the key did nothing. Astral Chain has no 2K modes behind it — the
run-time switchable part of the picture pipeline is the MetalFX output upscale — so key 3 now flips it on/off
(mfxup_output_set) and logs the state. In a 1:1 window both states look the same by design; the switch becomes visible
once the window or the full-screen draw exceeds the 1600x900 canvas. Verified live: the log flips mfx_output 0/1 per
press. (First press in that session showed "on" because the launch had TOTK_MFX_OUTPUT unset — off — by default.)

Also observed while capturing interpolated frames (TOTK_MFX_SHOT=<frames>:<path>): during the tutorial dialogue the
game frame dims the background but the interpolated frame shows it undimmed — the dimmer apparently rides somewhere
other than the scene canvas or the UI layer. Cosmetic, present in menus only; left open with the wshot issue.

**Screen-level verification of the gamma fix (user still saw the old symptom).** The player reported 30+30 flickering
and 60+60 overexposed while plain 60 looked right — the exact signature of the double-encoded interpolated frames, but
measured against the pre-fix session they were playing (the fix landed while that session was live; `flag mfx_interp`
keeps cycling modes inside a running process). Measured on the fixed build at the compositor level — full-screen
`screencapture` of the window eight times under interpmax and twelve times under 30+30: the sky region reads
identical (±1/255) across captures that necessarily include both game and interpolated frames, under both modes. The
fix holds on the screen, not just in the textures. One caveat for the next player report: ask which launch the
symptom was seen on before re-deriving color chains.

### 2026-10-30 (continued): interpolation in gameplay — the depth/camera chain fixed; the canvas is HDR, and that is what still washes the interpolated frames

Chasing the player's "still overexposed in gameplay, plain 60 is right" (the gamma fix had been verified on the title
screen only), a live A/B settled where the brightness lives: with the game focused and the window region captured,
interpolation off reads (53,60,79) and on reads (104,122,118) — every capture inside a mode identical, so the *shown
mix* is uniform and the game's own frames are untouched. Toggling the UI layer live (`flag mfx_ui`) changed nothing
(103.7 vs 103.4), so the wash is in the frame interpolator's own output.

Two real bugs were found and fixed on the way, both about *which* pass the interpolator's inputs come from:

- **The depth slot was being claimed by the wrong passes.** The rule "D32, no colour attachments, canvas size" first
  matched shadow atlases (2048x2048 — visible as the FI being rebuilt between 2048x2048 / 1280x810 / 1600x900), then
  the game's **UI-only depth pass** (also canvas size). The camera block read from those passes is the shadow / UI
  camera (identity or static), giving a motion-vector reprojection that never moved: `flag mvlog` reported median
  0.00 px on every frame while the player walked, i.e. every "interpolated" frame was a copy of the previous one —
  which is exactly the flicker/stutter the player described at 30+30. The claim now needs a depth texture of exactly
  the canvas size **and 4+ draws into it** (counted by depth serial, since the replay may split one game pre-pass into
  several Metal passes; pass numbers at setup are the previous pass). Verified in gameplay: camera reads a real pose
  (`pos -101.5 23.0 -70.8`, yaw 140.3°, VP row -0.887 0 0.735) and the motion texture is non-zero while the camera
  turns (`x 61.9 px` median on a 3.6°/frame step).
- **Camera reads from unclaimed passes polluted the state.** The ALC branch fell through to `mfx_take_camera()` even
  when no pass had been claimed, so menu/UI passes wrote identity cameras over good ones. The ALC path now only reads
  the camera at the claim.

The remaining wash is structural: Astral Chain's scene canvas is **RGBA16F HDR** (the game tone-maps while compositing
it into the presented RGBA8 texture), while the interpolator writes display-referred output; interpolating HDR values
and writing them without the game's tone map brightens and desaturates exactly as measured. The title screen was fine
because its background is a movie composited as display values. The fix is to snapshot the **presented** texture (after
the game's composite, before its UI draws) instead of the canvas — the same boundary the UI layer already uses — and
that is the next piece of work; it needs the pass split the canvas snapshot already does, moved after the composite
draw.

**Shipping decision:** the launcher's presets no longer enable interpolation (the control stays, marked experimental),
so a player cannot land in the washed mode by picking a preset. The 60 FPS patch, MetalFX output upscale and key 3's
master-switch fix (the key used to flip only the output stage while the scaler's master switch stayed closed — that is
why it "did nothing") are unaffected and remain on.

### 2026-10-30: internal resolution 1920x1080 (the game's own size config, nine sites)

Astral Chain renders at a dynamic size up to 1600x900 (docked) / 1280x720 (handheld). The docked pair comes from two
copies of the same chooser — `mov w9, #0x640` / `mov w9, #0x384` beside a `csel` against the handheld pair — plus the
display-size getter at main+0x4328c0 and three `cmp w0, #0x640` tests that decide "docked" by comparing the width.
scripts/make_res_patch.py (rewritten for this game; the by3 copy it replaced pointed at BY3_build/v120 and could not
run here) finds and patches all nine to 1920x1080, verifying each original word and decoding each replacement back
before writing mods/1.0.1/res-1920x1080/. The handheld pair and the dynamic-resolution arithmetic are untouched, so
the game's own levels still scale from the 1080p base.

**Verified**: with `ALC_PATCHES=60fps,res-1920x1080` the NVN swapchain is created at **1920x1080** (it was 1600x900),
the MetalFX output upscale runs 1920x1080 -> 3010x1686 at 0.74 ms/frame (1200/1200 frames), gameplay frame time stays
p50 16.67 / p95 16.94 / p99 17.05 ms at 60 fps with the game's own GPU work at p50 2.9 ms (about 1.5 ms at 900p —
still an eighth of the 16.7 ms budget), zero GPU errors and zero drops. A gameplay capture at 1080p shows the sharper
picture; note that edge-energy comparisons across two different scenes are not meaningful and were not used.

The launcher's Internal resolution control gains "Fixed 1080p" (setting 2), which selects res-1920x1080 when the
patch table is present; "Fixed 900p" still maps to res-1600x900 / force-scaling-900p-v3 when those exist. Run it with
`make patches` once to generate the table (14 rows of 60fps + 9 rows of res-1920x1080).

Not done here: no per-scene soak at 1080p yet, and the frame-interpolation HDR issue above is unchanged by this.

### 2026-10-30: the work folder rebuilt from scratch in this repository; the app ships

The old work folder lived in a second clone that was deleted by accident; everything was regenerated here and every
step came out byte-identical to the pinned values: ExeFS hashes for all four blocks okay; base RomFS through all six
IVFC levels (8,701 files, 10,309,560,892 bytes, SHA-256 cdfb7043…); the merged 1.0.1 RomFS 8,701 files / 10,310,254,332
bytes (182,747,996 bytes from the update); the rewrite (`DM_RULESET=native DM_PRIVATE_GATES=0`) sdk 10,957 sites and
main 5,426 sites with all four rewritten hashes matching `assets/expected-v1.0.1.json` exactly, and 87,192
relocations. `make prepare`, `make patches` (60fps 14 rows + res-1920x1080 9 rows), `make shader-tools` and
`make runtime` all ran clean; the rebuilt binary booted the title at 1920x1080 with both patches applied and MetalFX
output upscaling 1920x1080 -> 3010x1686 at 0.74 ms/frame.

**The app** (`make app GAME=<card> KEYS=<prod.keys>` then `make install`) puts Astral Chain.app in /Applications with
the game's own icon, running the game from `AC_build` (no data is copied). Three launcher fixes were needed to make it
usable for a player:

- **`TOTK_FREE_CORES=1` was missing.** The Makefile's boot target sets it (the task manager's user-space spin starves
  its own fiber without it — stage 3), but the launcher never did, so an app launch parked at frame 0 with no present.
  Verified after the fix: first presented frame 0.203 s through the app.
- **The window could open larger than the screen.** A 1920x1080-point window on a 1512-point-wide display opens partly
  off screen and a covered window stops presenting; the launcher now clamps the requested size to the screen's visible
  frame (1488x828 here) and the drawable keeps the backing scale, so MetalFX still gets 2944x1656 from the 1920x1080
  picture.
- **The presets now default to 60 fps with the fixed 1080p patch and MetalFX on** (Recommended and Quality, the latter
  full screen and without film grain); Original stays 30 fps as on the console, and the interpolation choices stay off
  (the HDR canvases still wash interpolated frames, see above).

`scripts/extract_shared_fonts.py` was missing from this folder (the icon script imports it); copied from games/by3
unchanged and recorded in third_party/COPIES.md.

### 2026-10-30: dropping to 30 fps ran at half speed — the step floor now follows the present pace

The player's "switching to the 30 fps limit slow-motions the whole game": with the 60 FPS patch the logic-step floor
is a patched 0.5 (half a 30-fps step per 16.7 ms frame — the point of the patch), but the floor must follow the
*pace*, not the work time. Pacing to 30 fps (interp "30+30", or any interval-2 mode) spaces frames 33.3 ms apart while
a frame's measured work stays ~17 ms (present waits are not counted), so the raw step is 0.5 and the constant 0.5
floor froze the logic at half speed.

The floor is now adaptive: `runtime_game_patches_note_present_gap(ms)` is called by the graphics layer at every
present (`present_now` reports the gap to the previous one), keeps a 32-sample median (one loading hitch must not
move it; gaps over 60 ms are ignored as loading), and derives floor = gap/16.67 × 0.5, clamped 0.5..2.0, quantized to
a quarter step. On a change it rewrites the three floor sites in place through `mach_vm_protect` (plain `mprotect`
returns EINVAL across the loader's sub-mappings — the Mach call handles the same range), flushing the icache. The
rewrite accepts the current floor's words, the target's, or the stock ones; anything else is refused with a log line.

**Verified in gameplay** (`flag mfx_interp 1` after reaching the field): floor transitions 0.5 → 0.75 → 1.00 as the
pace settles at 33.3 ms and the game runs at correct speed at 30 fps; back at 60 fps the floor returns to 0.5. Note:
while the game's dynamic resolution is below the canvas (1280x810 at the reduced level) the interpolation reports
missing depth and shows the game's own frame — no fill, no wash, and still correct speed.

On the way: `AC_build/extracted/v101/main.decompressed.bin` is produced by the NSO decoder in `prepare_modules.py`
(the raw layout parse in a scratch script was wrong); this analysis image is regenerated, not committed.

### 2026-10-30: the latest TotK settings / monitoring / key mapping merged in (mechanisms, not TotK's options)

Taken from `games/totk` as it stands (its copies are supersets of this folder's older ones):

- **Monitoring**: `fps_overlay.c/.h`, `totk_sysload.c`, `totk_timeline.c/.h`. The HUD has modes — all lines / FPS only /
  off — the FPS line shows the internal scene resolution in brackets ("FPS 60 3014x1688 (1920x1080) MetalFX已开启"),
  and a render-pipeline line ("新渲染管线 0 / 总计 404") sits under the shaders. nvn_mtl supplies the scene size from
  the learned canvas and publishes HUD_PSO; the corner box honours the modes.
- **Key mapping**: `input.c` (the player's keyboard and mouse layout through keyboard_map.h, `keyboard.txt` next to
  `controls.txt`; controller layout through controller_map.h; tap/toggle/hold modes; the diagnostic input-file
  commands), `controller_map.h`, `keyboard_map.h`, `gamepad_layout.h`, `mouse_look.h`, `switch_left_fallback.h`,
  `text_keyboard.h`, `bindings_diagram.h`.
- **Settings storage and feedback**: `gamepad.m` (runtime_setting_* reading/writing the text settings files, mouse
  mode, controller type, the settings button), `gamepad_feedback.m` + `hid_feedback.c` (rumble/gyro and their
  settings), `feedback_settings.h`, `hid_imu.h`, `playtime.h`, `game_data.c/h`, `game_language.h`, plus the headers the
  copied files need (aspect_math.h, aspect_patch.h, bow_state.h, contrib_submit.h, mem_scan.h, audio_ramp.h,
  app/update_check.h, update_notice.h, totk_fps_limit.h).

Astral Chain's own HID rows are re-attached in input.c's override table (older SDK: the single-state `GetNpadState`
imports, the touch screen answered as "no touches", the handheld activation mode, `ShowControllerSupport` by
reference), and the TotK-only picture APIs the copied files call are stubs in `game_patches.c` that answer "not
offered" (aspect ratio, output modes, render resolution, phone fps/motion).

**Not ported: the in-game settings panel.** Its rows are TotK's own options (2K modes, picture aspect, 40 fps
limits) — none exist here, and porting them would drag in that whole subsystem to show rows that do nothing. Key 1
therefore cycles the HUD instead; the launcher keeps the presets. When a panel is wanted for this game, its rows
should be Astral Chain's (interpolation mode, MetalFX output, HUD, key bindings), built on the settings storage above.

**Verified**: the game boots to the title with both patches (60 fps 14 sites, 1080p 9 sites), swapchain 1920x1080,
first presented frame 0.198 s, zero GPU errors; key 1 cycles HUD all -> FPS only -> off -> all (log lines and three
window screenshots); the HUD shows the four lines. Key bindings, mouse mode and rumble/gyro reach the game through the
merged files but were not exercised by hand yet.

### 2026-10-30: the two HUD features un-stacked, and the number keys restored

Two problems a player hit at once:

- **The monitoring lines and the shortcut list sat in one panel.** The compact corner box appended the key list under the
  FPS / shader / render-pipeline / load lines, so the two HUD features read as one duplicated block — and the list was
  TotK's text, naming rows this game does not have ("3 画质: MetalFX 2K", "4 21:9 宽屏"). The box now carries the
  monitoring lines only; the key list is its own overlay (the non-compact TOTK_HUD=corners view), and its text is this
  port's keys.
- **2, 3 and 4 stopped working.** The merged `input.c` came from TotK, where keys 2–7 are deliberately no shortcuts
  ("the panel replaces them"), so only key 1 survived the swap. The port's own dispatch is back on both input paths
  (window keys through the player layout, synthetic keys for scripts): 1 cycles the HUD (all / FPS only / off),
  2 cycles MetalFX frame interpolation, 3 toggles the MetalFX output upscale; 4 is left unbound (this game has no
  aspect patch — the key answers "not offered").

Verified live with the log and screenshots: key 1 -> `HUD FPS only` -> `HUD off` -> `HUD all` with the box showing
exactly the four monitoring lines and no key list; key 2 -> `mfx_interp = 1: 30+30`; key 3 -> `key 3: MetalFX output
upscale off`. One boot hit the known `reserve guest stack, heap and alias regions` race (macOS address layout, B4) and
succeeded on the retry.

### 2026-10-30: the in-game settings panel ported (key 1), on this game's options

TotK's panel (`runtime/settings_panel.h`, with `bindings_diagram.h`, `text_keyboard.h`, `playtime.h`) is now in, wired
through TotK's own `frontend.m` (the newer superset of this folder's, adopted whole — it defines the panel's host state,
the settings input view and the HUD box class the panel refreshes). The rows that are TotK's picture controls were
replaced or dropped, because this game has no equivalents:

- **Kept and real**: overlay mode (all / FPS only / off — drives the HUD), frame interpolation (off / 30+30 / half the
  refresh rate / every frame — `mfx_set`), frame lock (off / 30 / 40 / 60 / 72 / 90 / 120 — a real lock now: `nvn_fps_lock_set`
  paces the game at exactly that rate, and the adaptive logic-step floor follows the pace), controller type and the
  settings button, rumble / gyro settings, key + mouse bindings (the diagram page draws this game's actions), playtime,
  the contribution button, and the window/monitor rows.
- **Dropped**: the resolution/output-mode row (this port's internal resolution is an exefs patch the launcher picks;
  `runtime_output_modes()` answers 0), the aspect-ratio row (no aspect patch), edge smoothing (no host TAA), and the FPS
  graph toggle's TotK plumbing (the graph view itself comes with TotK's frontend).

Also ported for the frontend's sake: `totk_state_report.{h,m}` (the state-report diagnostic, with this fork's
`mtl_sr_picture` / `mtl_sr_counters` reporting the counters it has and 0 for TotK-only ones), `dm_gfx_fps_now` (the menu
bar FPS), `totk_shader_contrib.h`, and the contrib macros in `game_version.h` (`GAME_NAME`, `GAME_FOLDER` = ALC,
`GAME_LOG`, `GAME_CONTRIB_TEMPLATE`, `GAME_CONTRIB_TITLE`) that `contrib_submit.h` needs.

**Verified**: key 1 opens the panel over the running game and ESC closes it; the key-bindings page draws the full
keyboard + mouse map with this game's actions (锁定/举盾, 能力, 地图, 踩下, 攻击, 跳跃, 确认/互动, 取消/冲刺, 视角, 主视角) and the
controller page the same by position. With the panel closed, real keyboard events (System Events key codes) drive keys
2 and 3: `mfx_interp = 1: 30+30` and `key 3: MetalFX output upscale off` in the log. The Display and Input tabs could
not be clicked from the automation used here (synthetic clicks do not reach the custom view) — they need a hand click,
but their rows are built from the code above.

### 2026-10-30: the panel speaks in Switch buttons, and its dead rows are gone

- **Key caps and the action menu use the console's notation.** The keyboard page used to label each key with the game
  action it drives ("确认", "跳跃"); the player asked for the controller's own notation, so every key cap now shows the
  Switch button behind it — A B X Y, L R ZL ZR, + −, the d-pad arrows, LS / RS (with arrows for the stick directions) —
  and the action menu that opens on a click prefixes each row with that button ("A   确认 / 互动", "ZL   锁定 / 举盾", …).
  The action names stay where they read best: the menu rows, the list view and the "nothing drives them" line.
- **Dead rows removed from the Display page.** Resolution and aspect ratio were shown greyed with fixed values (this
  port's internal resolution is an exefs patch the launcher picks, and there is no aspect patch); they are left out now,
  so the page holds only rows that do something: Overlay, Frame lock, Frame interpolation. (The frame lock is real since
  this port: `nvn_fps_lock_set` paces at exactly the chosen rate and the adaptive logic-step floor follows it.)

### 2026-10-30: the save path and the shader seed follow the TotK/BotW scheme

Both are now the same mechanism the other ports in this repository use, so one set of habits (and tools) manages every
game:

- **Save and settings paths**: the launcher derived its data folder from a hardcoded "ALC"; it now reads `GAME_FOLDER` from
  `game_version.h` (added with the other shared names: `GAME_NAME`, `GAME_LOG`, `GAME_CONTRIB_*`), exactly as TotK's and
  BotW's launchers do. The layout is therefore the repository-wide one:
  `~/Library/Application Support/MacNativeGames/<GAME_FOLDER>/{profile,save,game-slot}`, profile holds the account and
  the save data (`profile/save/<account id>`), `game-slot` is the one-game-at-a-time slot. Existing saves stay where they
  were (GAME_FOLDER is "ALC"), so no migration was needed.
- **Shader seed**: the app can now carry prepared translations (`shader-seed/` in the bundle's Resources, made with
  `make shader-seed` from this machine's cache and shipped by `make app`), and the runtime reads them through TotK's own
  switch names: `TOTK_SHADER_SEED_DIR` (the launcher points it at the bundle) and `TOTK_SHADER_SEED=off` to skip it.
  On the first start the seed's files and their per-shader index files are imported into the session's cache once (a
  `.seed-imported` marker stops later walks of the folder); a lookup that still misses falls back to reading the seed in
  place, and anything new is translated as before by the runtime service. `make shader-seed` re-runs after playing to
  refresh what the app ships.

**Verified**: with an empty cache (`TOTK_SHADER_CACHE=/tmp/seedtest`) and the bundle's seed, the run imported 1062 of
1062 files and presented its first frame at **0.305 s** — the same cold start without a seed took 25.99 s when this work
folder was rebuilt that morning; on this machine's already-warm cache the import is a no-op (0 files, the seed was made
from it) and the boot is unchanged. The launcher's data folder resolves to the same path as before (GAME_FOLDER "ALC").

### 2026-10-30: the app carries everything and starts straight into the game

- **Self-contained bundle.** `make app` (now `SELF_CONTAINED=1` by default) copies the runtime into
  `Contents/Resources/work`: the rewritten modules, the merged 1.0.1 RomFS, the shared fonts, the patch tables, the
  shader tools and `build/alc-native` itself — 9.9 GB, cloned instantly with APFS clonefile. The launcher prefers that
  folder over the Info.plist's build-time path, so the app runs from anywhere (another Mac, a copy, a USB disk) with
  nothing else installed. `SELF_CONTAINED=0` keeps the old thin app. This also surfaced a Makefile bug worth recording:
  `$APP` in a recipe is `$(A)PP` to make, so the first run of the copy step wrote a stray `PP/` folder — the recipe uses
  `$(APP)` now.
- **Straight into the game, like the TotK app.** The settings window no longer appears at launch, not even on the first
  run: without saved settings the Recommended preset is written in place (60 fps, fixed 1080p, MetalFX) and the game
  starts at once. Launching with **Option held** still opens the window for presets, internal resolution, fullscreen and
  the mods; everything else (overlay, frame lock, interpolation, controller, key bindings, rumble/gyro) lives in the
  in-game settings panel on key 1.
- **Shader cache stays outside the bundle**: the launcher points `TOTK_SHADER_CACHE` at
  `~/Library/Application Support/MacNativeGames/ALC/shader-cache`, so a shipped app is never written to and the seed
  inside it remains the reference; the first start imports the seed (1062 files) into that cache.

**Verified** from a fresh state (defaults deleted, launched from Finder): no window, the log shows work =
`/Applications/Astral Chain.app/Contents/Resources/work`, both patches applied, seed imported 1062/1062, swapchain
1920x1080, saves in `MacNativeGames/ALC/profile/save/…`, and the game picture on screen within 4.7 s of the first
start (the import and first-run compiles included).

### 2026-10-30: player self-test results, and what is still open

The player took the new build through several chapters and reported back; recorded here as the state of the port:

- **1080p holds across several chapters**: no picture errors, no crashes (the 9-site resolution patch).
- **The in-game settings panel, key remapping (keyboard, mouse, controller) and mouse mode** work in play; **rumble and
  gyro were not exercised** yet.
- **Frame interpolation is not ready**: 60 fps is the recommended mode. Interpolated frames still wash in gameplay
  (the canvas is HDR; the fix is to snapshot the presented texture — known issue above), and the 30 fps choices are
  worse than that: **"30+30" and the 30 fps lock still run slow**.

**Root cause of the slow 30 fps modes (found in the player's log, 820 occurrences):**
```
[alc] step floor: raising the text to RW failed
```
The adaptive logic-step floor (the 60 fps patch's three immediates) is rewritten at run time through
`mach_vm_protect`. That works when the game binary is the one this folder builds, but **inside the ad-hoc signed
app it does not**: macOS enforces W^X on a signed executable's pages, so a page that the loader sealed read-execute
cannot be made writable again. The floor stays at 0.5 while the pace drops to 33.3 ms, and the game runs at half
speed — exactly what the player saw. The 25.99 s cold start note above is unrelated.

**Next session:** stop rewriting the guest text at run time. Two workable shapes: (a) apply the floor once at load,
while the text is still writable and before `seal()`, choosing it from the modes the launcher starts with and
telling the player a mode change needs a restart; or (b) replace the three immediates at load with stubs in the
existing code cave that read the floor from a writable word, so later changes only touch data. (b) keeps run-time
switching and is the intended fix.

Status of the port, for the record: **community port, early** — playable at 60 fps with a 1080p internal resolution;
interpolation and the 30 fps modes are still being worked on.
