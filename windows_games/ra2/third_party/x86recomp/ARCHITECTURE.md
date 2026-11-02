# Architecture

x86recomp turns a 32-bit x86 Windows game into a native 64-bit program:
the game's machine code is lifted to C++ ahead of time and linked with a
runtime that reimplements the Windows, DirectX and middleware APIs it
calls (HLE). Nothing here is specific to one game: game projects hold
that (entry point config, manifest, symbols, game hooks) and use this
tree as a library.

## Scaffold, not the destination
Recompilation is how a game first runs natively, not how it stays. The
end state of each port is native source: lifted functions are replaced
by hand-written C++ one subsystem at a time until nothing lifted is
left, and the lifter, guest arena and HLE fall away with them. That
shapes the design:
- The lifted code stays exact (integer flags, x87, SSE, MXCSR), so it is
  the reference every hand-written replacement is diffed against.
- Replacements must be able to take over any function by address and
  work on the same guest state while their neighbours are still lifted.
- Nothing that only the scaffold needs (CPUID answers, the guest heap,
  thunks) should leak into the native code that replaces it.

## Goals and hard constraints
1. Native Linux first; macOS on Apple Silicon with a native Metal
   renderer is a requirement, not an afterthought. Nothing in the lifter
   output or the runtime may assume an x86 host, 4 KiB pages, or
   Linux-only APIs outside a platform layer.
2. No emulation in the shipping binary: no interpreter, no JIT, no
   runtime x86 decoding, no fallback path. Every guest instruction that
   can execute is lifted to C++ ahead of time. An indirect branch to an
   address that was not lifted is a fatal error (reported with its VA so
   it can be added to the manifest), never a slow path.
3. No Wine, in any form, in the product: the Win32/DirectX surface games
   use is reimplemented natively. Wine (with the original EXE) is allowed
   only as a dev-time reference to compare behaviour, and is never
   linked or shipped.
4. 64-bit everywhere on the host: host code, the runtime and every
   library are 64-bit; nothing needs i386 multilib.
5. Users bring their own game: no game code or data in any repository.

```
game.exe ──Ghidra──> functions.tsv, jump tables ─┐
     │                                           ▼
     └──────── exports ──────────────>  recomp_lift (x86-32 -> C++)
                                                 │
                                  generated/*.cpp (gitignored, per game)
                                                 │
   ┌────────── game binary (native 64-bit, portable C++) ─┴───────────┐
   │ lifted game code ── guest arena (4 GiB, base chosen at startup)   │
   │        │ calls to import thunks / COM vtables (0x7F000000+)       │
   │        ▼                                                          │
   │ HLE: kernel32 user32 gdi32 advapi32 ole32 mss32 binkw32 ddraw ... │
   │        │          │                 │                 │           │
   │     POSIX       SDL3            gfx::Device       SDL3 audio,     │
   │                (window,     ├ d3d9 (dxvk-native)  ffmpeg          │
   │                 input)      └ SDL_GPU: Vulkan, Metal              │
   └───────────────────────────────────────────────────────────────────┘
```

## What "64-bit" means here
The host process, the runtime and all libraries are 64-bit. The game's
own data layout stays 32-bit: its structures contain 32-bit pointers,
its heap lives in a 4 GiB arena, and the lifted code computes 32-bit
addresses, because that is what the compiled machine code does.
Recompilation keeps the program's semantics; it does not retype its
data. The route to anything beyond that is incremental: replace lifted
functions with hand-written C++ one subsystem at a time.

The arena is not emulation: it is how the program's address space is
represented. A guest load is one add of a base register plus the load
itself, on both x86-64 and ARM64.

Host libraries (SDL3, dxvk/SDL_GPU, ffmpeg, libc) are ordinary 64-bit
libraries and never see guest addresses. The HLE layer is the only
place where the two worlds meet:
- Guest -> host: a guest pointer argument becomes `base + addr`
  (e.g. CreateFileA's path, a vertex array passed to DrawPrimitive).
  Little-endian on both sides, so data is used in place, no copies.
  Structs that contain pointers are read through guest-layout mirror
  types whose pointer fields are `u32`.
- Host -> guest: host objects are never exposed as 64-bit pointers.
  Kernel objects (files, events, threads) become 32-bit handles indexing
  a host table. COM objects are small guest-memory objects whose vtables
  point at thunk addresses and whose payload holds a host table index.
  Memory the game must read or write (Lock() results, MapViewOfFile
  views, heap blocks) is placed inside the arena.

## Game projects
A game project adds this repository with `add_subdirectory()`, links
`recomp_runtime` plus the optional HLE modules its EXE imports
(`recomp_mss32`, `recomp_bink`, `recomp_d3d7`, `recomp_dbghelp`), adds
its lifted code with `recomp_add_lifted()`, and fills a
`recomp::GameConfig` in its `main()`:
- `id`: per-user data directory ($XDG_DATA_HOME/<id>) and the prefix of
  dev/test switches (<ID>_TRACE, <ID>_TEST_INPUT, ...);
- `guest_dir`, `exe_name`: where the game believes it is installed;
- `registry`: what the retail installer would have written;
- hooks for the rare behaviour that needs game knowledge (e.g.
  `game_cursor` for games that re-centre the pointer);
- `fullscreen`: how a window covering the game's screen is shown (see
  "Display modes, fullscreen and presentation").

The game's executable holds its lifted code, its replacements and
`main()`; the runtime, the x87 model, the HLE modules and the libraries
under them are shared libraries (librecomp_*.so, .dylib on macOS), so
each has one copy of its state (the HLE registry, the x87 model's
switches, the guest arena) and a change to one rebuilds only it. With
CMAKE_LINK_DEPENDS_NO_SHARED the game is not relinked either: a change
that keeps the libraries' interface needs no game rebuild at all. The
lifted code and the inline headers it includes (lifted.h, ctx.h, x87.h)
are the exception: changing those still recompiles every game.
- A library cannot refer to the executable's symbols (a dylib must not;
  `--no-undefined` holds Linux to the same), so the lifter's function
  table reaches the dispatcher through `RegisterLiftedFunctions`, called
  before `main()` from runtime/lifted_table.cpp, which
  `recomp_add_lifted()` compiles into the lifted code.
- HLE modules are found by nothing but their registrars: the runtime
  makes its consumers link `--no-as-needed`, so a linker that drops
  unreferenced libraries keeps them.
- Thread-locals use the initial-exec TLS model: the fault handler reads
  them, and a general-dynamic access may allocate.
- Logging (runtime/log.h) is in the base library, recomp_image, under
  the x87 model and the runtime; the runtime adds the guest state,
  backtraces and crash file to FatalF through `g_fatal_report`.

Game-specific patches to lifted code (fixes, enhancements such as
widescreen) live in the game project as hand-written replacements of
specific lifted functions. `replace <va>` in a manifest makes the lifter
emit that function's body as `f_<VA>_lifted`; every call site and the
dispatch table still reach `f_<VA>`, which the game project defines (in
`recomp::gen`) and which may call the lifted body.

## Decisions

### Guest arena
One 4 GiB reservation; guest VA A lives at host `base + A`. The image is
mapped at its real VAs (0x400000..), so every constant address in the
code is valid unchanged. `base` is chosen at startup (preferred
0x10_0000_0000, which is free on Linux and outside macOS's 4 GiB
__PAGEZERO and main-image range; macOS may place it elsewhere, the hint
being advisory there) and read from a global by lifted code.

Win32 memory semantics (4 KiB commit granularity, 64 KiB allocation
granularity, page protections, VirtualQuery) are tracked in a per-guest-
page table. Host protection is only "accessible or not" at host page
granularity (4 KiB on x86 Linux, 16 KiB on Apple Silicon and Asahi), so
nothing depends on host page faults for guest behaviour, with two
exceptions: lazy regions (below, Lazy guest memory), and games that demand-page reserved memory from an exception
filter (commit and fill one page per access violation). On a host with
larger pages, committing one guest page makes its whole host page
accessible, and the other guest pages in it would never fault. So after a
handler continues from a fault, the uncommitted guest pages of the same
reservation in that host page are faulted in too (read faults at their
first byte), before the faulting instruction is retried.

### Platform layer
src/runtime/platform.{h,cpp} holds what POSIX does not cover the same way
on Linux and macOS; the rest of the runtime has no OS #ifdefs:
- the guest main thread's stack switch (ucontext, deprecated but working
  on Darwin, where it needs _XOPEN_SOURCE);
- decoding a fault's signal context (write bit, pc, caller);
- leaving a fault with a C++ exception. glibc's unwinder steps through
  the kernel's signal frame, so a guest exception raised while an access
  violation is delivered (an __except unwind) is thrown straight from
  the handler. Darwin's unwinder cannot leave _sigtramp: there the
  handler catches the exception, rewrites the signal context so that the
  thread "calls" a rethrow function from the faulting instruction, and
  returns. That makes the faulting function an ordinary caller frame,
  which needs a frame record even in leaf functions: lifted code and the
  x87 model are built with -mno-omit-leaf-frame-pointer on Apple hosts
  (the link register is overwritten);
- stat's time fields and the per-user data directory
  (~/Library/Application Support/<id> on macOS).

### Address-space headroom: 2 GiB -> ~4 GiB (planned, opt-in)
EXEs that are not large-address-aware never saw an address at or above
0x80000000 on Windows, and their code may rely on that. Until audited
per game, the guest allocator hands out only addresses below 0x80000000
(the thunk region at 0x7F000000 stays there either way). Then:
1. Audit for code that breaks with high addresses: signed compares or
   sign-extending shifts on pointers (`cmp ptr; jl/jg`, `sar`, `cdq`
   on addresses), "negative means error" checks on pointer-or-error
   returns, top-bit tagging of pointers, pointer differences stored in
   signed ints, and address-to-int conversions in hashing/sorting.
2. Add a top-down allocation stress mode (like Windows'
   AllocationPreference=MEM_TOP_DOWN test for LAA apps): every
   VirtualAlloc/heap/MapViewOfFile result is placed as high as possible,
   so bugs show up at startup instead of hours into a session.
3. Fix what the audit finds with manifest overrides or host
   replacements, then allow allocations up to 0xFFFE0000 behind a config
   flag. It becomes a game's default only after a full playthrough in
   stress mode.

### Lifter: x86-32 -> portable C++
No maintained tool lifts Win32 x86 PE to portable C. Early-2000s MSVC
code is integer + x87, ~190 mnemonics, few SEH frames, so a purpose-built
lifter is tractable (rexglue-style: one C++ function per guest function).

- Decoder: Zydis (fetched via CMake FetchContent), build-time only.
- Function list: Ghidra functions.tsv + exports + a per-game line-based
  manifest (lifter.manifest) for overrides: extra entries, names, entry
  probes, resume sites (SEH, longjmp), setjmp functions.
- Output is plain C++20: no x86 intrinsics, no inline asm, no
  `long double`, no reliance on host FPU state other than the default
  environment (x87 runs on host doubles).
- Context: `struct Ctx { u32 eax..edi, esp, eflags; X87 fpu; fs base }`,
  one per guest thread. Registers are cached in locals per function and
  flags computed eagerly into locals (clang drops dead ones).
- Flags: the local `F` is one bool per flag, but Ctx holds them as one
  EFLAGS word (PackEFlags). Every call spills them with one 32-bit store
  and reloads them with one 32-bit load, which every x86 and arm64 core
  forwards from store to load. Flags a function does not touch between
  the reload and the next spill fold to `eflags & mask | 0x202`.
  - Ctx used to hold the bools, and spill and reload copied the 9-byte
    struct. SROA cut each function's copy into widths of its own (one
    qword, i32+i16+i16+i8, single bytes), so a function that loaded the
    flags as a qword or a word just after its callee had stored them as
    bytes could not forward them. That happened after nearly every call
    and at function entry, and the load waited for the stores to retire.
  - The flag helpers (Add, Sub, Logic, ..., PackEFlags, UnpackEFlags)
    are always_inline. In a large switch, clang rates every call site
    cold (inline threshold 45) and left some of them out of line. One
    out-of-line helper takes F's address, which keeps F in memory for
    the whole function: every flag write becomes a byte store, and every
    spill loads those bytes back.
  - Measured on Zen 5 (ls_bad_status2.stli_other): the change removes
    44% of a game's store-to-load conflicts outside its frame limiter.
    Work per frame drops 2.7% in a frame-capped game; in another, the
    frame time changed by less than run-to-run noise (-0.3 to -1.1%).
- Registers in Ctx: each guest register (and eflags) has an 8-byte slot
  of its own (alignas(8) on a uint32_t). With adjacent fields, clang
  merged spills and reloads of neighbouring registers into 64-bit or
  vector accesses (`movdqu (ctx)` for eax..ebx, `movq 0x14(ctx)` for
  ebp+esi): the backend combines adjacent 32-bit loads and stores, and
  the SLP vectorizer packs them into vectors. The function on the other
  side of the call used 32-bit accesses, so neither direction forwarded.
  A gap between the fields leaves no two accesses that can be merged.
  - Measured on Zen 5 after the flags change: store-to-load conflicts
    halve again in one game, whose frame time drops 0.6%. In a
    frame-capped game they drop 20% outside the limiter, and its work
    per frame does not change measurably.
- Calls: direct `call` -> C++ call; the real return address is pushed to
  the guest stack so code that reads it (__alloca_probe, SEH) still
  works. `ret N` pops. Tail `jmp` to another entry -> tail call.
- Indirect calls/jumps: dense table indexed by (va - .text start);
  addresses in the thunk region go to HLE handlers; anything else is
  fatal (constraint 2).
- SEH: the fs:[0] chain lives in guest memory as on Windows.
  RaiseException walks it and calls the lifted handlers. Resuming at a
  continuation (catch/except target) throws a host exception carrying
  {target va, ebp, from va}, caught by the lifted function that owns the
  target label.
- Non-local exits (`resume <va>` in the manifest, on a call or an
  indirect jmp): the call or jmp throws GuestResume{target, ebp, from}
  with the guest registers spilled to Ctx, instead of calling. Every
  function that writes fs:[0] or calls a setjmp wraps its body in
  try/catch; the activation whose EBP local equals the thrown ebp catches
  it and runs the target with Resume(): Dispatch to the target entry,
  repeated for any later GuestResume into the same frame (one thrown
  from inside the catch handler would otherwise leave the function's try
  block). The continuation is a separate lifted function; its final
  `ret` returns through Dispatch and the owning function returns, so the
  host stack keeps mirroring the guest's.
  - SEH: the site is __except_handler3's call into the __except block,
    which starts with `mov esp, [ebp-18h]`; the target is an entry
    because the handler table holds its address.
  - setjmp/longjmp: `setjmp <va>` names the CRT's setjmp (e.g. MSVC's
    __setjmp3). The return address of each direct call to it becomes a
    strong entry (the caller's body tail-calls it after setjmp returns
    0) and the caller gets the try/catch. `resume <va>` goes on longjmp's
    final `jmp [jmp_buf+14h]`, reached after it has unwound the SEH chain
    and restored ebp/ebx/esi/edi/esp from the jmp_buf, so the thrown ebp
    is the setjmp caller's. Matching by EBP assumes the setjmp caller has
    a frame of its own or that no deeper function with the same EBP also
    catches; an FPO setjmp caller inherits its caller's EBP. Calls to
    setjmp through a pointer are not recognised.
- Oracle tests (dev-time only): every x87 and integer instruction form
  found in a game EXE is run lifted and natively from the same random
  state (tests/lift_*_test.cpp, `recomp_add_lift_tests`).

### Multiple modules
Games that ship their logic in DLLs they LoadLibrary themselves (e.g.
separate game, client-game and UI modules) have every module lifted:
- One lifter run takes the EXE plus each DLL (`--module`, with its own
  symbols and manifest). The images are mapped side by side as at run
  time, so a VA, and the f_<va> name of its lifted function, identifies
  its module; table.cpp lists them all. Pointer scans only accept
  targets in the module the pointer was found in.
- A DLL loads at its preferred base. If two modules overlap, the lifter
  refuses and one gets `--base`: it is lifted from its relocated image,
  and the game's GameConfig names the same base, because lifted code has
  the module's addresses compiled in. A mismatch is caught at startup
  (the DLL's entry point is not in the function table).
- GameConfig::lifted_dlls lists them. All are mapped at startup (their
  ranges are reserved before the allocator hands out memory); imports
  are bound to HLE thunks like the EXE's. The dispatcher keeps one dense
  table per module's .text.
- LoadLibrary counts references and runs DllMain under a loader lock;
  GetProcAddress returns the real export VA (by name or ordinal),
  GetModuleHandle/GetModuleFileName know the module while it is loaded.
  After the last FreeLibrary (DllMain detach) the next load maps the file
  afresh, so the DLL starts from its initial data as after a real unload.
- Static TLS in a lifted DLL is refused: Windows XP did not set it up for
  LoadLibrary'd DLLs, so DLLs of that era have none.
  DLL_THREAD_ATTACH/DETACH are not sent (not needed so far).

### x87: full 80-bit model
Games depend on x87 state (some run all their float maths under
PC=24/RC=chop; MSVC CRT helpers switch precision around their own work;
D3D changes the control word), so the FPU is modelled exactly. Host
doubles are used only where they provably give the same results (x87
runs on host doubles, below).

Model per thread: 8 registers of 80 bits (sign, 15-bit exponent,
64-bit explicit-integer mantissa), TOP, an empty bit per register,
control word (PC, RC, exception masks) and status word (C0-C3, sticky
exception flags, stack fault). The other tags (valid, zero, special) are
not stored: they always equal the class of the register's contents
(every write sets it, and FLDENV/FRSTOR keep only "empty" from the
image), so the tag word is derived when FNSTENV/FNSAVE store it;
x87_test compares stored tag words with the CPU's. Semantics to honour:
- PC rounds the significand of fadd/fsub/fmul/fdiv/fsqrt (and their
  pop/reverse/integer-operand forms) to 24/53/64 bits while the exponent
  keeps the full 15-bit range (so PC=24 does not flush to float range).
  Loads, stores, transcendentals and fprem are unaffected by PC.
- RC applies to every rounding: arithmetic, fst/fstp to m32/m64,
  fist/fistp, frndint.
- fist/fistp out of range stores the integer indefinite (0x80000000 /
  0x8000 / 0x8000000000000000); masked invalid sets IE.
- fcom/fucom/ftst/fxam set C0/C2/C3 for the `fnstsw ax; sahf` idiom,
  NaN/unordered included.
- New threads start with the Windows default CW 0x027F (PC=53,
  RC=nearest, all exceptions masked), independent of the creator.

Implementation: Berkeley SoftFloat 3e `extF80` (BSD-3; has
`extF80_roundingPrecision` 32/64/80 and all four rounding modes, and is
portable C, so it works on ARM64 where `long double` is only 64 bits).
SoftFloat has no transcendentals: fsin, fcos, fsincos, fptan, fpatan,
fyl2x, fyl2xp1, f2xm1 and fscale have our own extF80 implementations
(reference: Bochs' x87 code, LGPL, read only).

Fast paths are integer-only (no host FPU) and provably identical to the
SoftFloat path they bypass, which stays the reference
(tests/x87_fast_test compares them; x87_test and lift_x87_test compare
against the hardware):
- add/sub/mul/div of normal or zero operands: exact 128-bit result,
  rounded by the model's RoundPack;
- the two modes games run in, with the operands they produce, inline
  (`run::Fast24` and `run::FastWide`, x87.h, shared by the model and
  lifted runs): PC=24 (any RC) add/sub/mul/div of 24-bit significands,
  exact in 64 bits (a quotient keeps its remainder as the sticky bit),
  and of up to 53-bit ones (m64 operands); PC=53 (any RC, Windows'
  default) add/sub/mul/div of up to 53-bit significands (m32/m64 loads
  and PC=53 results), exact in 128 bits. A zero operand is exact. They
  give up (to the paths above) on specials, denormals, exponents too far
  apart, results leaving the normal range and division by zero;
- FLD/FST m32/m64 of zero and normal values (FST rounding with RC, the
  result staying normal), and FCOM of zero and normal values (an integer
  compare of exponent and significand), inline;
- fsin/fcos/fsincos: series in 128-bit fixed point, used only when the
  result is further than 2^-100 from any rounding boundary.

### x87 runs (lifter)
Calling the model per instruction made every instruction a serial
read-modify-write of TOP, the empty bits and the status word in memory,
and games run millions of x87 instructions per frame (CPU skinning,
particles). The lifter therefore turns straight-line x87 code into runs:
- A run is a stretch of one basic block (no labels inside, no calls or
  branches; other instructions may be interleaved) of x87 instructions
  of a supported subset (loads, stores, the arithmetic forms, fxch,
  fchs, fabs, fsqrt, fcom family, ftst, fnstsw). A single instruction is
  a run too: its guard and write-back cost less than the State-based
  call.
- A symbolic stack (X87Sim) resolves every st(i) to an entry-relative
  slot at lift time; slots become locals (`run::V`, padding-free so they
  stay in registers), fxch and fld st(i) become copies.
- One guard at the start (`run::Guard`): all exceptions masked, every
  slot the run reads before writing is non-empty and every slot it
  pushes into is empty. Then no instruction of the run can fault, so
  the value-level `run::` functions (the same code the State-based
  operations use) give exactly the per-instruction results; sticky flags,
  C1 and C3/C2/C0 are tracked in locals; fnstsw inside a run builds the
  status word from them.
- At the end the registers, empty bits, TOP and status word are written
  back. If the guard fails the original per-instruction code runs
  instead (it is emitted in the else branch).
- The fast paths above (arithmetic in the two common modes, m32/m64
  loads and stores, compares) are inlined into the lifted code; the
  rest are calls.
- The control word has a 32-bit field of its own: a run's write-back
  stores the status word, the next run's guard loads the control word,
  and a load widened over both missed store forwarding (5% of a hot
  function).
- What a run costs is the latency of its dependent operations (about
  5 ns each through the integer paths, about 1 ns on host doubles), not
  the guard and write-back (~2.5 ns per run), so keeping values in
  locals across branches would gain little.
- <ID>_X87_RUNS=0 disables runs at run time (A/B checks);
  <ID>_X87_DOUBLE=0 only their host-double form.
- Known difference from hardware, the same one lifted integer registers
  already have: while a run is in flight its state is in locals, so a
  guest exception raised inside it sees the FPU state from the start of
  the run in its CONTEXT, and an __except unwind out of the run drops
  the run's changes.

### x87 runs on host doubles
Most float code runs under Windows' default control word 0x027F (PC=53,
RC=nearest, exceptions masked); a game that never sets the CW does all
its float maths there. Under it a run with arithmetic first runs on host
doubles (x87.h, "host doubles"; lifter: EmitX87Double):
- After the guard, if PC=53 and RC=nearest (run::DoubleMode), the
  registers the run reads are converted to doubles d<k> (run::ToD). The
  same instructions then run on the doubles, with PE, C1 and C3/C2/C0 in
  the run's usual locals.
- Each instruction checks before it has any effect. Its operands must be
  held exactly (see "Range"), and its result must stay in range. If a
  check fails, the run saves the instruction's index (rk) and jumps to
  XS. There the doubles become registers again (exact), and the run
  continues on the exact path at that instruction (label XR), which
  redoes it. If a register cannot be converted at entry, the whole run
  takes the exact path.
- At the end the doubles are converted back and the normal write-back
  stores them.
- Stores of a double do not exit: an m32 store outside float's normal
  range uses the exact store. FIST and the 64-bit constants (fldpi...)
  go through the exact model; FLD1 and FLDZ are 1.0 and 0.0.

Range. Every value a double holds is zero, or has a magnitude in
[2^-511, 2^512). Such a value has at most 53 significand bits and is a
normal binary64, so the double and the 80-bit register hold the same
number. Values enter only as:
- registers that are already like that (ToD);
- m32 operands (zero or normal floats are in range);
- m64 operands in range;
- FILD with |v| <= 2^53, FLD1 and FLDZ.
NaN, infinity, denormal and out-of-range operands exit.

Proof that a result in range is exact. Take a and b in range, and the
operation fadd, fsub, fmul, fdiv or fsqrt (and their reverse, pop and
integer-operand forms). Let x be the exact result.
1. Zero results. If x = 0, both give the same signed zero: IEEE 754 and
   the x87 use the same rules at RC=nearest (x - x = +0, -0 + -0 = -0,
   products and quotients take the xor of the signs).
   A nonzero x is never that small:
   - products are at least 2^-1022;
   - quotients are greater than 2^-511 / 2^512 = 2^-1023;
   - square roots are at least 2^-256;
   - a nonzero sum is a multiple of the smaller operand's quantum,
     2^-563 or more.
   So binary64 never rounds a nonzero x to zero, and a zero double
   result means x = 0.
2. Nonzero results. The x87 at PC=53 rounds x once to 53 bits, and its
   15-bit exponent cannot overflow or underflow here. Binary64 rounds x
   the same way when x is in its normal range; below 2^-1022 it gives a
   denormal, at 2^1024 and above infinity. The run keeps a result r only
   if |r| is in [2^-511, 2^512).
   - Rounding is monotonic and powers of two are representable. So
     x below 2^-1022 gives |r| <= 2^-1022, and overflow gives infinity.
     Both are rejected.
   - An accepted r therefore comes from a normal-range x and equals the
     x87 result.
   - Conversely, an x87 result in range is accepted.
   - A run exits exactly when the x87 result leaves the range. No other
     exception can occur: operands are normal or zero, and division by
     zero gives infinity or NaN, which are rejected.
3. PE and C1. Let e = x - r. PE is e != 0, and C1 ("rounded up" in
   magnitude) is e != 0 with sign(e) != sign(r). With operands and r in
   range, e is computed exactly:
   - Add/sub: TwoSum (Knuth) gives r + e = a + b exactly when no
     operation overflows. Operands below 2^512 keep every intermediate
     below 2^514.
   - Mul: e = a*b - r is a multiple of 2^(ea+eb-104), with
     ea + eb >= er - 1 >= -512. It fits in 53 bits (|e| <= ulp(r)/2), so
     fma(a, b, -r) computes it exactly.
   - Div: the remainder p = a - r*b is representable when r is the
     correctly rounded quotient and nothing underflows (Markstein;
     Boldo and Daumas). Its quantum is 2^(er+eb-104) with
     er + eb >= ea - 1. fma(-r, b, a) computes it exactly.
     x - r = p/b, so PE is p != 0. Since sign(r) = sign(a) xor sign(b),
     C1 is p != 0 with sign(p) != sign(a). Comparing with sign(r)
     instead would be wrong for negative divisors.
   - Sqrt: p = a - r^2 is a multiple of 2^(2er-104) with er >= -256.
     fma(-r, r, a) computes it exactly; C1 is p < 0.
   No error term is smaller than 2^-616 unless it is zero.
4. Stores and compares.
   - FST m64 of a value in range is exact: no PE, C1 = 0.
   - FST m32 rounds the 53-bit value once to 24 bits, as (float)r does
     at RC=nearest. That holds while |r| >= 2^-126 (the x87 checks
     tininess before rounding) and the float is finite; otherwise the
     exact store runs. PE is (double)f != r, and C1 is |f| > |r|.
   - Compares see no NaN, so host compares give C3/C2/C0; +0 and -0
     compare equal on both. FCOM m64 accepts any zero or normal value.
   - FCHS, FABS, FXCH and register copies are exact.

Host requirements:
- binary64 evaluated as written: FLT_EVAL_METHOD 0 and no fast-math
  (x87.h refuses to compile otherwise);
- no contraction into FMA: everything that runs these paths is built
  with RECOMP_X87_FP_FLAGS;
- round-to-nearest-even, the host default, never changed;
- masked traps (a division by zero computes infinity before the check
  rejects it).
No denormal is ever an operand or a result, so FTZ and DAZ could not
change anything, but platform::HostFpProblem rejects them anyway. Every
thread that runs guest code checks it at start: the rounding mode, the
control register (MXCSR on x86-64, FPCR on arm64), and the behaviour.
std::fma is a single instruction on arm64. On x86-64 without -mfma it is
glibc's fma, which is correctly rounded too.

Tests and measurements:
- tests/x87_fast_test fuzzes every function against the SoftFloat path
  at 0x027F. A function must succeed exactly when the exact result can
  be held, and then match value, flags and C1 bit for bit. The cases
  include range edges, ties, cancellation, signed zeros, results just
  over and under the range, float range edges for m32 stores, and any
  m64/m32/register bits for the loaders.
- Two counterexamples to weaker conditions are kept as cases:
  - "normal operands, result >= 2^-1022" is not enough:
    6361 * 2^-600 times 1416003655831 * 2^-475 is (2^53 - 1) * 2^-1075.
    Binary64 rounds it to 2^-1022 (its denormal quantum); the x87 holds
    it exactly.
  - a zero result is not exact without the range: 2^-512 * 2^-791
    gives 0.
- Mutations of each check, error term, flag and conversion make the
  test fail.
- lift_x87_test runs the lifted forms and sequences from 0x027F states
  that are often double-exact, against the CPU.
- Lifter output grows by 7-16%: the run is emitted twice, plus the exits.

### SSE/SSE2
Every PC these games still run on has SSE2, so their SSE paths (MSVC's
SSE2 maths library, engine SIMD loops) are what they actually execute;
CPUID reports a fixed SSE2-class CPU (Pentium M, family 6 model 13),
never the host's: lifted code must behave the same on every machine,
Apple Silicon included (it has no CPUID), and nothing is claimed that
the lifter does not implement (no SSE3+). The XMM registers and MXCSR
live in Ctx and are used in place (never cached in locals).
- Float lanes go through SoftFloat with the SSE NaN rules applied in
  runtime/sse.cpp (the shared SoftFloat build carries the x87 ones):
  one NaN operand is returned quieted, two give the first source, SNaN
  raises IE; invalid operations give the default NaN. Rounding control,
  every sticky flag (IE DE ZE OE UE PE) and precedence (a zero divisor
  suppresses DE for a denormal dividend) match the hardware.
- Not modelled, fatal if reached: unmasked SIMD exceptions (#XM), and
  FTZ/DAZ actually changing a result.
- Integer and data-movement forms are plain portable C++.
- tests/lift_sse_test runs every SSE form in a game EXE lifted and
  natively from random states rich in special values (for a game whose
  EXE has 244 forms: 2.44M checks, all bit-exact).
- In the native port these sites go away: CRT maths becomes the host
  libm, engine SIMD becomes portable C++ (or SIMDe/sse2neon where speed
  matters), validated against the lifted reference.

### Graphics: API HLE over gfx::Device
Games' graphics APIs are implemented as HLE over one backend-neutral
fixed-function device, `gfx::Device` (src/runtime/gfx/device.h, D3D7
semantics: render and texture-stage states, transforms, lights, FVF
draws). Backends:

1. d3d9 via dxvk-native (Linux bring-up). D3D9 still has the D3D7
   fixed-function pipeline and DXVK implements it, so this backend is
   mostly 1:1 state translation and gets pixels on screen fastest. It
   is also the reference image for backend 2.
2. SDL_GPU (src/runtime/gfx/gpu_device.cpp): Vulkan on Linux, native
   Metal on macOS. This is the long-term backend on both platforms.
   dxvk-native cannot be it: DXVK on macOS would need MoltenVK, which
   lacks features DXVK requires, and would not be native Metal anyway.
   Design:
   - The fixed-function pipeline is one vertex and one fragment
     uber-shader (gfx/shaders/ff.vert, ff.frag) driven by uniforms
     holding the D3D state: vertex blending, lighting (directional, point, spot;
     material sources), texgen and texture transforms (projected too),
     vertex and table fog, all texture-stage ops and arguments, alpha
     test, flat shading, user clip planes. Nothing is compiled per state.
     The GLSL is the only source: glslang builds SPIR-V for Vulkan, and
     for Metal SPIRV-Cross converts a build with Metal's buffer order
     into MSL; both are embedded at build time.
   - Vertices are pulled from a storage buffer with the FVF's offsets in
     the uniforms, so the vertex layout is not pipeline state. Pipelines
     (blend, depth/stencil, rasterizer, topology, depth format) are
     cached by state; triangle fans become lists.
   - D3D draws arrive one at a time with their data in guest memory, and
     SDL_GPU uploads only outside render passes. Draws are therefore
     recorded per render target (vertex/index data and uniforms in CPU
     arenas) and flushed as one copy pass plus one render pass when the
     target changes, a texture in use is updated, or on readback and
     present. A full-target Clear before any draw is the pass's load op.
     So a readback in the middle of a frame (a game blending a software
     cursor on the CPU) is what submits the frame, and the CPU waits for
     all of its GPU work. Splitting the pass earlier so the GPU starts
     sooner does not pay on tile-based GPUs: the extra store and load of
     the target is cheap (~0.1 ms at 5120x2880 on an M3 Pro), but hidden
     surface removal works within a pass, so everything an early part
     draws gets shaded even where later draws cover it (one 5K frame:
     11 ms of fragment work whole, 16 ms split seven ways, measured with
     Metal System Trace). Instead the device can read back without
     waiting (StartReadBack / FinishReadBack) and blend an ARGB image
     itself (BlitBlend), for hooks that move such CPU work to the GPU.
   - Colour textures and targets are B8G8R8A8 (the memory layout of D3D's
     A8R8G8B8); other formats convert on upload and readback, as the d3d9
     backend does for its X8R8G8B8 back buffer. DXT stays BC1-3. Blends
     reading destination alpha see 1 on targets without alpha.
   - D3D9's integer pixel centres become SDL_GPU's half-integer ones
     with a half-pixel shift in the vertex shader. SDL_GPU flips Vulkan's
     viewport, so clip space is y-up on both APIs.
   - Direct3D 9 shaders (translated from HLSL, see Shaders below) are
     compiled when created: the backend prepends its prelude (bindings
     in SDL_GPU's layout, vertex pulling by declaration type from the
     same storage buffer, D3D9's pixel centres, the alpha test), glslang
     makes SPIR-V and, on Metal, SPIRV-Cross MSL. Their draws record
     like the fixed-function ones: each stream's vertex range is copied
     to the arena, the constant registers are uniform buffers, and the
     pipeline is cached by state, shader pair and target formats (up to
     four targets). Front faces stay D3D's clockwise ones whatever is
     culled, so VFACE keeps its sign.
   <ID>_GFX=d3d9|gpu selects the backend; the default is SDL_GPU, with
   d3d9 kept (where the build has it) as the reference image (it has no
   programmable pipeline: DXVK would need the shaders as D3D bytecode).

API front ends:
- DirectDraw 7 / Direct3D 7 (recomp_d3d7): guest-visible COM objects;
  DirectDraw (surfaces, flipping chain, Lock/Blt, colour keys, display
  modes) implemented in the HLE itself. Locks of the back buffer and
  primary read back lazily (Lazy guest memory, below).
- OpenGL 1.x: opengl32/wgl HLE as a GL state machine on gfx::Device,
  not a pass-through to host OpenGL, so the same Metal path serves it
  (macOS OpenGL is deprecated and capped at 2.1 for legacy contexts).
- Direct3D 9 / D3DX 9 (recomp_d3d9, in progress): device, resources and
  state in the HLE, sent to gfx::Device at each draw; draws with
  translated shaders (from HLSL or bytecode) on its programmable
  pipeline, draws without shaders on gfx::Device's fixed-function one
  (D3D9's sampler states and transform numbers mapped to D3D7's). Mixing
  a shader with the fixed-function pipeline, fixed-function draws with a
  vertex declaration, and instancing are not done yet (fatal and named).

### Shaders: HLSL translated from source
Games that build HLSL at run time and compile it with D3DXCompileShader
get it translated from source by src/runtime/hlsl (HLSL to GLSL; a
graphics backend compiles the GLSL with glslang, and SPIRV-Cross for
Metal, as for the fixed-function shaders but at run time). Translating
the source rather than compiled bytecode keeps the Direct3D 9 interface
the game relies on:
- Uniforms live in D3D's constant registers (c, i, b, s). Explicit
  `register()` bindings are honoured, including deliberately overlapping
  ones; the others are placed in free registers away from every explicit
  binding (read or not, since games set registers by number), and the
  ID3DXConstantTable describes that placement. The GLSL reads the
  register file (`vec4 c[n]`), so the application's register writes need
  no translation.
- Inputs and varyings keep D3D semantics: vertex inputs are fetched by
  slot through the backend (`r_fetch`, so the vertex declaration is not
  pipeline state), varyings sit at a fixed location per semantic, so
  any vertex shader pairs with any pixel shader.
- HLSL's implicit conversions (scalar splat, vector and matrix
  truncation) are made explicit by the checker; matrices are GLSL
  matrices whose columns are the HLSL rows (mul reverses its operands);
  log, rsqrt, sqrt and pow take the absolute value of their argument as
  the D3D9 instructions they compile to do; non-static globals the
  shader writes become private copies (D3DX accepts that).
- D3DXCompileShader returns a shader function holding the translation in
  a comment (version token, comment, END), which Create*Shader
  recognise.
- Shaders shipped compiled (shader model 1.1-3.0 tokens) go through a
  second front end to the same GLSL contract (runtime/hlsl/bytecode.cpp):
  registers become GLSL variables, each instruction a vec4 expression
  with the source swizzles and modifiers, then the shift / saturate
  result modifiers, write mask and predication; co-issued pairs read
  before they write; subroutines become functions and rep / loop / if /
  break GLSL's own; def'd constants override the register file, also
  for relative reads. ps_1_x texture-matrix and bump instructions and
  ps_1_4 are not supported (named).
The translator stops at the first construct it does not support, so a
gap is a fatal error naming it, never a silently wrong shader.
<ID>_SHADER_DUMP=dir saves every source, and recomp_hlslc translates a
saved source (or dumped bytecode, .vso / .pso) and reports the
reflection; tests/hlsl and tests/hlsl_bytecode_test run their shaders
through glslang.

D3D7 FPU side effect: the HLE must reproduce whatever the real runtime
does to the guest thread's CW, because the game's own float maths runs
on that state. DDSCL_FPUSETUP = the app keeps the FPU in D3D's preferred
state; DDSCL_FPUPRESERVE = D3D saves and restores the CW around each
call; without either, the Windows runtime switches the thread to single
precision and leaves it there. Wine's ddraw does not model that default,
so Wine is not a reference for this point. d3d7.cpp models it.

### Lazy guest memory (locks of GPU surfaces)
Games lock the DirectDraw back buffer and primary, which live on the GPU.
Reading a whole surface back on every Lock costs a GPU sync and, at
desktop resolutions, tens of MB per frame (5120x2880x4 is 59 MB), and
games often lock without touching anything: Lock(back buffer, DDLOCK_WAIT)
then Unlock is a common way to wait for the GPU. So their guest memory
mirrors the GPU image lazily (src/runtime/lazy_mem.{h,cpp}, used by
ddraw.cpp):
- The surface memory is a VirtualAlloc-style reservation of its own,
  rounded to host pages, so no other guest data shares its host pages.
- Lock protects its pages (PROT_NONE) and records the rect. Nothing is
  read yet.
- The first access to a protected page faults. The fault handler offers
  every fault in the arena to the lazy regions first (before guest SEH,
  on any thread, from lifted or host code): the region reads the locked
  rect's rows covering that page back from the GPU, copies the rect bytes
  of that page into guest memory and a snapshot, unprotects the page, and
  the handler returns, which retries the faulting instruction (on Darwin
  too: no unwinding, nothing thrown).
- Pages are 4 or 16 KiB and rows are pitch bytes; neither divides the
  other in general. A pull reads whole rect rows but writes only the
  pages it opens, since an open page may already hold the guest's
  writes. A guest walking the rect would fault once per page, so each
  fault of a Lock pulls an aligned block twice the size of the last
  (about log2(pages) read-backs for a sweep in either direction); a rect
  of at most 1 MiB (a cursor, a sprite) comes in whole on its first
  touch.
- Unlock compares the pulled bytes with the snapshot. If nothing was
  touched there is nothing to compare or upload. Otherwise the band of
  rows the guest changed is completed (its unpulled pages pulled) and
  uploaded, as before.
- DDLOCK_READONLY skips the compare. DDLOCK_WRITEONLY is treated like a
  normal lock: the bytes of a written row that the game leaves alone
  must go back unchanged, so they are still read.
- Pages stay protected after Unlock until touched; touching one outside
  a lock opens it with whatever it held (DirectDraw leaves that
  undefined). Host code that reads or writes the surface memory itself
  (Blt from or to these surfaces) settles the rect it uses first: during
  a lock the locked bytes in it are pulled, then its pages are
  unprotected. Blt from them reads back only the source rect.
- The kernel does not fault on a protected page, it fails the syscall
  (EFAULT), so HLE handlers that pass guest buffers to the kernel
  (ReadFile, WriteFile) call lazy::Touch on them first.
- Threads: regions change state under one lock, which the fault path
  takes too, so a cursor thread drawing on the primary while the main
  thread unlocks the back buffer is serialised. A page is unprotected
  before it is filled, so another thread touching the same page in that
  window would read stale bytes; DirectDraw gives a locked surface to
  the thread that locked it, and games do not share it.
- A fault on a lazy page while that thread holds the region lock (a
  runtime bug) is not handled but reported like any other fault.

### Display modes, fullscreen and presentation
Display modes are virtual: the host's mode never changes behind the
game's back. What games see and what the host shows:
- Modes (EnumDisplaySettings, ChangeDisplaySettings, DirectDraw's
  EnumDisplayModes; one list, `DisplayModeSizes()`): the classic 4:3 and
  5:4 sizes, common wide ones (1280x720 to 3840x2160) and the desktop's
  own size, up to the desktop's size, in 16 and 32 bpp. A mode switch
  makes that the screen the game sees (GetSystemMetrics, ...).
- Host fullscreen: a visible top-level window that covers the whole
  screen the game sees (a popup at a switched mode, a borderless window
  at the desktop's size, DirectDraw exclusive mode) goes fullscreen on
  the host, and back to a window of its client size when it no longer
  does. `GameConfig::fullscreen` (<ID>_FULLSCREEN=window|desktop|
  exclusive) picks how: a host window of the mode's size (the old
  behaviour; the default for <ID>_BACKGROUND runs), borderless at the
  desktop's resolution (the default), or a real host mode switch. For
  exclusive, the host mode asked for is the game's mode widened to the
  display's aspect (4:3 1024x768 on a 16:9 display asks for 1366x768 and
  gets the smallest 16:9 mode that holds it), so the monitor or the
  compositor (Wayland emulates mode switches by scaling) never stretches
  it. The cursor clip and focus handling are those of any window: a
  clip is in the game's screen coordinates, whatever the host window's
  size.
- Presentation: the frame (the game's back buffer: the display mode, or
  a window's client area as the game sees it; OpenGL's default
  framebuffer has that size too, not the host window's) is shown scaled
  to fit the host window with its aspect kept and black bars around it
  (`gfx::FitRect`), in every backend's present (SDL_GPU: a clear and a
  blit into the fitted rectangle; d3d9: a swap chain of the window's
  size, recreated with a device Reset when the window changes size).
  The bars appear when the mode's aspect differs from the screen's (4:3
  on 16:9 is pillarboxed) or the window manager gave the window another
  shape.
- Pointer: absolute positions map back through the same rectangle
  (positions over the bars clamp to the edge of the game's screen).
  Relative motion that drives a virtual cursor (clipped mouse look,
  exclusive mode) is not scaled: one unit of host motion is one pixel of
  the game's screen, as a mickey is on Windows, so mouse look turns the
  same in a window, fullscreen and in any mode.
- Pixels and points: everything games see (modes, the desktop size,
  screen metrics, client areas, so render resolutions) is in pixels; host
  windows are sized in points, which differ on a scaled desktop (macOS,
  scaled Wayland outputs: a 3840x2160 panel at 1.5x is a 2560x1440-point
  desktop). Windows are created with high pixel density, so a client of
  w x h pixels is a host window of w x h pixels, w / d x h / d points;
  the swap chain is the window's pixel size. Pointer positions (points)
  map through the fitted rectangle in point space.
- For game patches (runtime/display.h): `DesktopSize()` (the host
  desktop in pixels) and `gfx::LastPresentation()` (the last frame's
  size, the output's size and the fitted rectangle, so the output's
  aspect). Deciding what to do with them (FOV, where 2D goes) is the
  game's.

### Frame pacing
Old games limit their frame rate by spinning on a millisecond timer,
which burns a core and only reaches rates of 1000/n. `FramePacer`
(runtime/pacing.h) is for game patches that replace such a limiter:
Wait(fps) at the start of each frame sleeps until the next frame's start
on a grid of exact periods on the host's monotonic clock. Lateness does
not add up (the next start is a period after the previous one, not
after the wake-up), a frame that ends late is followed by a shorter
wait, and one more than a period late moves the grid instead of causing
a burst. The sleep is the platform layer's `SleepUntilNs`: an absolute
clock_nanosleep with the thread's timer slack at 1 us on Linux;
mach_wait_until on macOS, with the thread under a time-constraint
(real-time) policy only while it waits (a timeshare thread's timer gets
coalescing leeway: it woke 1-3.5 ms late, whatever its QoS class,
latency tier or task role). No spinning. tests/pacing_test checks the
rate (within 1%), the median wake-up lateness, the CPU use and the
late-frame rule; on an idle host wake-ups land 15-30 us (Linux) or
20-80 us (macOS, Apple silicon) after the deadline.

### HLE surface
- kernel32: heap/virtual memory in the arena, files with case-insensitive
  path mapping onto the game dir, MapViewOfFile as mmap into the arena,
  threads (one Ctx + guest stack each), events, mutexes, critical
  sections, TLS (TEB at a fixed guest VA, fs:[0x2c]), time, locale.
- user32/gdi32: SDL3 windows; SDL events translated to WM_* and fed to
  the guest WndProc through PeekMessage/DispatchMessage.
- advapi32: registry backed by a small file, seeded per game.
- ole32: CoCreateInstance for the classes HLE modules register (fails
  cleanly otherwise); oleaut32 BSTRs.
- mss32 (Miles): our own mixer on an SDL3 audio stream (runtime/audio):
  resampling, loop counts, gain ramps, the "Miles Fast 2D Positional
  Audio" model of the game's Miles version (6.1c or 6.5; docs/miles.md),
  MP3 through libavcodec. Miles' EAX providers (EAX 1.0, 2.0, 3.0, with
  Msseax.m3d's attribute names and struct layouts): voices pan like
  DirectSound3D hardware and get an LF level and a 5 kHz level
  (a two-pole low-pass) on the direct path and the send from the EAX
  source model (runtime/audio/eax), and the send goes to a reverb bus:
  our environmental reverb (runtime/audio/reverb), rebuilt from the
  card's measured impulse responses with textbook parts (tapped delay
  lines for the reflections and the start of the late reverb, a
  feedback delay network with Jot-style absorption filters for the
  rest, an echo comb). The EAX presets are the card's own read-backs.
  The Fast 2D path is unchanged. Miles 6.5's digital
  master reverb (room types, reverb time/predelay/damping, driver and
  sample/stream wet levels) is our Miles-compatible Schroeder reverb
  (runtime/audio/digital_reverb), written from a behavioural description
  of mss32 6.5e's (docs/miles.md) and bit-exact with it:
  recomp_add_digital_reverb_test lifts the original routines from the
  user's installed DLL at build time (`recomp_lift --only`) and compares;
  tests/digital_reverb_test checks a hash of our output on every host.
  runtime/audio is its own library (`recomp_audio`, no HLE);
  tools/eax_capture renders with it offline to compare against
  recordings of real Creative hardware.
- Measured against hardware (an Audigy 2 ZS under Windows 98 SE through
  Miles 6.5e's Msseax.m3d; tools/eax_capture): where EAX leaves behaviour
  to the card, the model follows that card (details next to the code):
  the HF low-pass, occlusion on the room path, size scaling, EAX 1.0's
  table and controls, read-backs, DS3D panning, air absorption, and the
  reverb's structure: reflection and late-burst tap patterns, the tail's
  start and level, decay per band, the broader low-pass on the send.
  Still different (hardware minus render over the capture, mean |d|: RT
  mid 0.12 s, RT 4 kHz 0.11 s, EDT 0.37 s, onset 1 ms, early 0.9 dB,
  late 1.3 dB, occlusion and air on the reverb 0.3-0.4 dB): very damped
  small rooms (carpeted hallway, living room, the 1 m environment size)
  have 7-10 dB less late reverb than the card; padded cell's early
  reverb is 7 dB low and psychotic's 5-7 dB high; echo depth is a comb
  at the echo time, where the card shortens the burst and makes the
  tail periodic (alley, echo depth cases); reverb +2000 mB is 2 dB
  louder than the card's (it seems to limit there); modulation's pitch
  deviation is a guess; the outdoor presets with long delays (forest,
  mountains, plain) are barely measurable; EAX 1.0's damping and
  decay-time effects rest on two settings each.
- dsound (DirectSound 8): on the same mixer, as Windows Vista and later
  implement it (software only). Buffers keep a host copy of their
  samples, updated at Unlock, and play it as a ring; the write cursor is
  how far the mixer has read ahead. DirectSound's volume and pan law;
  DS3D buffers and the listener (deferred settings, rolloff) use the
  DS3D panning model above. Cones and Doppler are stored only;
  notifications and effects are not implemented.
- xinput1_3: SDL gamepads in the Xbox controller's layout, user indices
  in connection order; none in automated runs. DirectInput lists the
  same pads as game controllers, and WMI (Win32_PNPEntity DeviceIDs with
  "IG_") marks them as XInput ones, as games that pair the two (the
  DirectX SDK's IsXInputDevice) expect.
- winmm: multimedia timers on one timer thread, as on Windows (TimeProc
  callbacks, or SetEvent / PulseEvent on the given event).
- binkw32: ffmpeg (Bink demuxer from memory, binkvideo/binkaudio,
  swscale), audio into the software mixer whichever sound system the
  game picks (BinkOpenMiles or BinkOpenDirectSound). The 1.x API up to
  Bink 1.8's: sound tracks by ID (BINKSNDTRACK, the track list in the
  BINK struct), per-track volume, BinkCopyToBufferRect, BinkGoto,
  BinkGetRealtime.
- dbghelp: UnDecorateSymbolName (LLVM's Microsoft demangler, normalised
  to MSVC's output); the rest stubbed.
- shfolder, wsock32 (host-name queries), imm32, version: small shims.

Every import needs an explicit handler; an unhandled one is fatal by
design. All of it is written against POSIX + SDL3 + gfx::Device, with
platform specifics (mmap flags, paths, page size) in one place.

### Distribution: users bring their own EXE
The lifter is a permanent build stage: a game's logic in the port is its
output, so it is re-run whenever its semantics, the manifest or the
function list change. It never ships inside the binary. What can be
published: this library, game projects (manifests, hooks, replacements,
docs). What is never published or committed: game EXEs/DLLs/data,
generated/ (lifted C++ is derived from the EXE), symbols/, Ghidra
projects, memory dumps, extracted archives.

Licence hygiene: dxvk (zlib), SDL3 (zlib), SoftFloat 3e (BSD-3), Zydis
(MIT), CORE-MATH (MIT) are fine to vendor or link. ffmpeg is LGPL (built without GPL
parts): link dynamically. Wine and Bochs (LGPL) and GPL engine source
releases are references only: read them to learn behaviour, do not copy
code from them.

### Memory ordering on weakly ordered hosts (macOS arm64)
x86 is TSO; ARM64 is weakly ordered. Lifted code reaches guest memory
with plain loads and stores, so data shared between threads without a
lock would be correct on x86-64 hosts and racy on Apple Silicon.

- Lifted `lock`-prefixed instructions and xchg are seq_cst atomics. On
  Apple targets (LSE always present) they compile to `casal`, `ldaddal`,
  `swpal`, which are fully ordered, like the x86 instructions. Built
  for an ARMv8.0 target without LSE they would be LL/SC loops, which are
  not full barriers for the surrounding plain accesses.
- HLE synchronisation calls are full barriers, as on Windows (a locked
  instruction or a kernel entry there). The host mutex or condition
  variable behind most of them gives acquire/release between threads
  (what one thread wrote before releasing is visible to the next one
  that acquires), but lock + unlock is not a full barrier: accesses
  outside it may move into the critical section and pass each other.
  So each handler opens a `GuestSyncPoint` (kernel32_state.h): a seq_cst
  fence on entry and on exit, early returns and ExitThread's unwind
  included. That covers SetEvent, ResetEvent,
  CreateMutexA (it may take ownership), ReleaseMutex,
  WaitForSingleObject, WaitForMultipleObjects (and through it
  MsgWaitForMultipleObjects), InterlockedIncrement/Decrement/Exchange
  (seq_cst RMWs, which C++ orders only against other seq_cst atomics),
  CreateThread, ResumeThread, ExitThread, TlsAlloc, TlsFree and Sleep
  (polling loops). A thread's start fences before its first guest
  instruction (pthread_create already publishes the creator's writes)
  and its exit fences before it is signalled. TlsGetValue/TlsSetValue
  touch only the caller's TEB and are plain accesses on Windows too.
- EnterCriticalSection/LeaveCriticalSection run Windows XP's algorithm
  on the guest structure: each is one seq_cst read-modify-write of
  LockCount (`ldaddal`/`casal` on Apple targets, a locked instruction
  on x86-64), fully ordered like the guest's own, so no separate fence;
  a contended Enter waits on a host semaphore that the releasing Leave
  signals.
- Window messages (PostMessageA, PeekMessageA, GetMessageA) go through
  the user32 queue mutex: acquire/release, so a message's payload
  written before posting is visible to the receiver.

Still open: a lock-free hand-off in a game's own threads (a plain flag
polled without any of the calls above) needs explicit barriers via the
manifest once one is found.

### 64-bit guests (x86-64, PE32+) — in progress
A mode of the same lifter, runtime and HLE, not a second codebase. The
mode is the image's machine (0x14C or 0x8664); one process is one mode.
Everything above holds unless this section says otherwise.

Guest address space:
- x64 images have fixed bases above 4 GiB (0x140000000 for EXEs, which
  usually have their relocations stripped, 0x180000000 for DLLs), so
  guest addresses are 64-bit. The arena is one reservation of 2^N bytes,
  N per process: 32 for 32-bit guests (unchanged), 36 (64 GiB) for x64
  by default, raised through GameConfig if a game needs more. Guest VA A
  is still host `base + A`; `gaddr` is uint64_t.
- x64 guests get nothing below 4 GiB: heap, stacks, TEBs, thunks and
  module handles live above it, as on Windows with high-entropy ASLR. A
  pointer truncated to 32 bits anywhere (in the HLE above all, which
  kept pointers in uint32_t) then faults at once instead of aliasing
  other memory. From the top of the 64 GiB arena (runtime.h): the thunk
  region (its upper half holds the HLE's data exports), the pseudo
  module handles, the PEB, 16 MiB of TEBs (two pages each) growing down;
  the allocator hands out [4 GiB, TEBs).
- The PEB and TEBs have the x64 layouts (TEB Self +0x30, TLS array
  +0x58, PEB +0x60, LastErrorValue +0x68, TlsSlots +0x1480); x64 guests
  see Windows 10 22H2. Lifted x64 DLLs may have static TLS (Vista and
  later set it up for LoadLibrary'd DLLs): each gets a TLS index after
  the EXE's, and every thread a block for each.
- Lifted code adds A to the base without masking. A wild pointer outside
  the window would reach host memory instead of faulting; guest pointers
  come from the guest's own image, stack and allocator, so this needs a
  guest bug that would also crash on Windows. The reservation is
  followed by a PROT_NONE guard of the same size to catch overruns.

Context: the register slots become uint64_t rax..r15; the 32-bit names
are the low halves of the first eight (an anonymous union), so 32-bit
lifted code and the HLE keep using eax..edi with the same 32-bit
accesses. XMM grows to 16 registers, with the upper ymm halves in an
array of their own (legacy SSE code never touches it). x64 code reaches
its TEB through gs (Ctx::gs_base; fs is unused).

Lifter:
- Zydis in long mode for PE32+; VAs are 64-bit throughout. Lifted
  functions are still f_<VA> (eight hex digits or more), so 32-bit
  output does not change.
- Registers are uint64_t locals (lifted files define RECOMP_LIFT_X64,
  which switches lifted.h's register caching and the runtime entry
  points to 64-bit addresses). 32-bit writes zero-extend, 8/16-bit
  writes merge (with REX: spl, bpl, sil, dil and r8b-r15b; no ah), and
  a zero bsf/bsr source leaves even a 32-bit destination's upper half
  alone, as the CPU does. The flag helpers gain an 8-byte width, in
  `if constexpr` branches of their own so the 1-4 byte forms compile as
  before.
- RIP-relative operands resolve to absolute VAs at lift time.
- push, pop, call and ret move 8 bytes; the real return address is
  still written to the guest stack (stack walks and unwinding read it).
- Function list: every .pdata (RUNTIME_FUNCTION) entry without
  UNW_FLAG_CHAININFO starts a function; chained entries are fragments of
  their parent (cold code split off by the compiler), reached by jumps,
  and lifted into it. Leaf functions have no .pdata entry and are found
  as before (calls, pointer scans, exports, Ghidra).
- Jump tables in the x64 MSVC form (`lea rB, [__ImageBase]; mov(sxd)
  eT, [rB + idx*4 + table_rva]; add rT, rB; jmp rT`, plus the byte-index
  variant) hold 32-bit RVAs and are recovered like the 32-bit forms, the
  index widened (movsxd, cdqe) between the bounds check and the load.
  MSVC drops the bounds check when it knows the index is in range (a
  state machine's state variable). Such a table is read while its
  entries target the jmp's own function (by .pdata, chained fragments
  included), up to the end of the .pdata range that holds it: MSVC puts
  a function's tables inside its range, after the code. Two adjacent
  tables then read as one, which only adds labels that are real case
  targets of the same function.
- Function pointers in x64 code are RIP-relative leas into .text (weak
  entries, like the immediates of 32-bit code); data pointers are
  8-byte aligned qwords.

Win64 ABI in the HLE:
- Arguments in rcx, rdx, r8, r9 (xmm0-3 for float ones), the rest at
  [rsp+0x28] onwards at the thunk; the caller owns the 32-byte shadow
  space and pops, so a handler's nargs means nothing for x64. Arg(c, i)
  follows the process's ABI and returns uint64_t, ArgF the float
  registers. Handlers return uint64_t (rax); 32-bit thunks keep the low
  half in EAX.
- Existing handlers keep guest pointers and structure fields in
  uint32_t. A handler is declared with HLE64 once audited (pointers in
  gaddr, x64 structure layouts); calling one that is not from x64 code
  is fatal and names it, like calling an unimplemented import. Mirror
  types of structures with pointers or handles (8 bytes in Win64) get an
  x64 layout next to the 32-bit one.
- Kernel handles stay small integers (Win64 handles are pointer-sized
  but their values fit in 32 bits, sign-extended: HandleRet; arguments
  are read as their low half).
- Data exports (C++ static members such as std::locale::id, globals a
  DLL exports) are guest memory the import binder points IAT slots at
  (hle::DataExport).

Exceptions: x64 SEH is table-based, not an fs:[0] chain.
- Each function's .pdata entry and UNWIND_INFO describe its prologue
  and name a language handler (__C_specific_handler, __CxxFrameHandler4
  and 3, __GSHandlerCheck*). The runtime implements the unwinder
  (RtlLookupFunctionEntry, RtlVirtualUnwind, RtlUnwindEx,
  RtlCaptureContext, RaiseException's dispatch) over the guest's unwind
  data and guest stack. The stack holds real return addresses and the
  prologues' pushes of non-volatile registers, so unwinding through
  lifted frames recovers them exactly.
- After an x64 prologue RSP does not move (pushes happen only there;
  alloca functions use a frame register), so a frame is identified by
  its establisher frame (RSP after the prologue, or the frame register's
  value). A resume (catch continuation, __except block) throws
  GuestResume keyed by that value instead of EBP, caught by the lifted
  function that owns the frame. Only functions with a handler in their
  unwind info get the try/catch.
- Catch funclets, __except filters and termination handlers are
  separate .pdata functions that the language handler calls with the
  establisher frame; they are ordinary lifted functions. Continuation
  addresses (from FuncInfo3/4 metadata and C_SCOPE_TABLE targets) are
  to be strong entries, read by the lifter from the handler data. Not
  done yet: a resume to one that is not an entry for another reason is
  the unlifted-address fatal, which names it.
- Software exceptions (throw, RaiseException) arrive through an HLE call
  with the registers spilled, so their CONTEXT is exact. Open: hardware
  faults inside __try ranges, where the faulting function's RSP and RIP
  are in locals; the scope tables say exactly which instruction ranges
  need them published.

CRT and C++ runtime: VCRUNTIME140, MSVCP140 and the UCRT api sets are
DLL imports in these games and get explicit handlers like any other
(vcruntime's C++ exception handlers parse MSVC's metadata, above), in
the recomp_ucrt library; the api-ms-win-crt-* sets resolve to
ucrtbase.dll as on Windows. The CRT's process data (argv, environment,
onexit tables, errno) is guest memory the guest reads through the
pointers it gets. MSVCP140 is HLE too, not Microsoft's DLL: objects the
guest constructs (mutexes, condition variables, streams, locale facets)
keep MSVC's layouts, with host state keyed by their address and, for
classes with virtual functions, guest vtables of thunks. The elementary functions (sin, pow, atan2f...)
are correctly rounded (CORE-MATH, cmake/coremath.cmake): the same on
every host, where the UCRT's own are not correctly rounded and the host
libms differ; they may differ from Windows in the last bit. Functions
with exact results (sqrt, floor, fmod, ldexp...) are the host's.

AVX: games test CPUID and XGETBV and take AVX paths when they are
offered, and console targets on x86-64 are AVX CPUs, so VEX code is
lifted, not avoided:
- VEX-encoded 128-bit forms and 256-bit AVX lift onto runtime/sse lanes
  (SoftFloat with the SSE NaN rules, exact MXCSR flags). A VEX.128 write
  zeroes bits 255:128 of the destination; legacy SSE leaves them alone.
- The YMM upper halves live in Ctx::ymm_hi; VEX instructions are lifted
  one 128-bit half at a time on the same lane functions (AVX has almost
  no operation that crosses halves; vperm2f128, vinsertf128 and
  vextractf128 move whole halves).
- CPUID for x64 processes is a fixed CPU of its own, never the host's:
  AMD family 16h (the Jaguar class, which the console targets also
  have), with only the feature bits the lifter implements bit-exactly
  with oracle tests set, growing towards that part's set (SSE3, SSSE3,
  SSE4.1/4.2, POPCNT, LZCNT, AVX, F16C, BMI1, MOVBE, AES, PCLMUL; no
  AVX2 or FMA). AVX is claimed (OSXSAVE, XCR0 = 7 through XGETBV) once
  its forms hold. Games read CPUID themselves (MSVC's CRT fills
  __isa_available from it), so which paths they take follows from this
  table.
- Oracle tests (tests/lift_x64_test.cpp) run every integer and SSE/AVX
  form found in an x64 image natively and lifted from random states:
  all 16 general registers, the full YMM registers, flags, MXCSR and a
  memory buffer. The native stub keeps its state pointer in a
  RIP-relative slot, so forms may use any register but rsp.

Graphics, audio, platform: Direct3D 11 / DXGI (and D3DCompiler_47) as
HLE over gfx::Device; XAudio2 on the mixer; SDL2 imports on host SDL3;
online services (store and multiplayer SDKs) get explicit handlers that
report being offline.

Order: lifter first (PE32+, .pdata, long-mode emission, oracle tests),
proven on a DLL with few imports; then the 2^N arena, Ctx and the Win64
HLE ABI; then the unwinder and CRT; then graphics and audio.

## Roadmap
- 64-bit guests (above).
- EAX for games on DirectSound3D (IDirectSound3DBuffer property sets),
  on the same EAX model and reverb buses as the Miles providers.
- SDL_GPU backend: matched image for image against the d3d9 backend;
  bump mapping (BUMPENVMAP) and PREMODULATE, which the uber-shader only
  approximates.
- macOS arm64: app bundle.
