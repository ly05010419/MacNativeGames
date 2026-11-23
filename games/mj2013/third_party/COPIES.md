# Where this folder's code comes from

明星三缺一 2013 runs on the Zelda-line runtime: the rewriter, the host gate, the threads and synchronisation, and the
in-process IPC server that Tears of the Kingdom, Odyssey, Mario Kart 8 Deluxe and Red Dead Redemption use. It does **not**
use nxnative code (the maintainer, 2026-11-21: "我们的项目不用 nxnative 代码，统一用塞尔达的代码，不维护两套").

The game is a libnx homebrew, not a Nintendo SDK game: one module (`main`), no imports, no NVN. It talks to Horizon only
through SVCs and IPC, so none of the Zelda line's by-name SDK replacements (`runtime_override`) or its NVN-on-Metal graphics
layer apply. What it shares is therefore linked, and what is new is the IPC-level answers a homebrew needs. The closest
base is Mario Kart 8 Deluxe (`games/mk8dx`, the newest port on the shared scheduler and IPC server); taken 2026-11-22.

| Here | From | Changed here |
|---|---|---|
| `third_party/d2r` (link) | `games/totk/third_party/d2r` (MIT, the rewriter) | two additions in the rewriter itself, both inert for SDK modules: an empty `eh_frame_hdr` (libnx links without unwind tables) leaves no function boundaries and each site is rewritten alone; in a libnx module (`HOMEBREW` at +8) the crt0 header words and MOD0's `LNY` records are data, never rewritten (`LNY1` decodes as `adds w12, w18, ...`) |
| `runtime/ipc.{c,h}`, `threads.c`, `gate.S`, `thread_enter.S`, `thread_compat.h`, `hle_stack.c`, `input_file.h` (links) | `shared/runtime` | nothing |
| `runtime/hle_stack.S` (link) | `games/odyssey/runtime/hle_stack.S` (the copy 21 ports share) | nothing |
| `runtime/audio_out.c` (link) | `shared/runtime/audio_out.c`, new in `shared/` on 2026-11-22: the audout:u server that seven ports keep as identical copies (by3, kirby, mk8dx, odyssey, rdr, unicorn, xcx) | nothing |
| `runtime/mng_active.h` (link) | `games/totk/runtime/mng_active.h` (every game's copy is that file) | nothing |
| `third_party/d2r_gfx/gfx/tex/nvn_deswizzle.{c,h}`, `gfx/nvn/nvn_tex.{c,h}` (links) | `shared/third_party/d2r_gfx` | nothing; only the block-linear addressing (`nvn_bl_offset`) is used |
| `runtime/loader.c` | `games/mk8dx/runtime/loader.c` | one module, no relocation (libnx relocates itself; RELR adds the base, so applying it twice would break), entered at crt0 with x0 = 0 and x1 = the main thread; the guest's `CTR_EL0` reports 32-byte lines; libnx's stdout and stderr reach the log; diagnostics `hbregs` |
| `runtime/svc_memory.c` | `games/mk8dx/runtime/services.c` (address space, heap, stack mirrors, GetInfo) | shared memory and code memory mapped at the guest's address, QueryMemory from a list of mappings |
| `runtime/frontend.m` | new | one Metal texture shown in the window; keys, mouse as touch |
| `runtime/hb_*.c`, `services.c`, `input.c`, `fb_convert.*`, `hid_publish.*`, `sdmc_path.h`, `keyboard_layout.h`, `game_patches.*` | new | — |
| `third_party/libnx/hid_shmem.h`, `LICENSE.md` | switchbrew/libnx `nx/include/switch/services/hid.h` at b156b477 (ISC): the type part, unchanged, with a small prelude | — |
| `tests/fb_probe.c`, `tests/hid_probe.c` | contain libnx's `_convertToBlocklinear` / `_convertGobTo16Bx2` and `_hidGetStates` (ISC) as the reference the port is checked against | — |
| `scripts/prepare_mj2013.py` | new; extraction through `games/pla/scripts/extract_exefs.py` / `extract_romfs.py` as they are | — |

The protocols (sm, set:sys, appletOE, hid, time, fsp-srv, vi, the binder relay and IGraphicBufferProducer, nvdrv) follow
switchbrew's public descriptions and how libnx (ISC) calls them; no Horizon or SDK code is used.
