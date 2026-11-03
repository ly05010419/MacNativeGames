#pragma once
#define GAME_PSV 1
#define GAME_TITLE_ID 0x01008F6008C5E000ULL
/* Inherited from games/ot0 (a game that reserves 62 GiB): the alias region above 2^39. Kept from games/xc3 until Pokémon Violet's
   own address space request (main.npdm) is checked. */
#define GUEST_ALIAS_HIGH 1
/* No per-version addresses yet, on purpose (as in games/dread): an address left over from another game must not compile.
   Each one is added here, per version, once it is found and checked in this game's own code. */
#if defined(PSV_GAME_V400)
#define PSV_GAME_VERSION "4.0.0"
#define PSV_GAME_DIR "v400"
/* The two clock-advance functions (main+0x92dd20, main+0x2b7b308): adrp x8 + ldr x8, [x8, #0xbc0] load the frame-duration
   table (16666667, 33333334, 50000000, 66666667, 83333334 ns) that the frame function (main+0x2b79f60) indexes by its
   step, 1 in the 30 fps mode. runtime/game_patches.c points these two at its own table. */
/* The game's frame-rate mode, as the Switch 2 runs it at 60 fps: step byte [[main+0x4741210]+0x30]+0x20 (the frame function's
   step: 1 = 30 fps, 0 = 60 fps; the game then asks for present interval 1) and the time-scale index
   [main+0x47350b8]+0xd0 (int32, set to 1 by its constructor at main+0x140ef10), by which three systems divide the clock. */
#define PSV_FRAME_OBJECT_GLOBAL 0x4741210
#define PSV_FRAME_RATE_GLOBAL 0x47350b8
/* The three time-scale readers (main+0xb83d24, main+0xd96430, main+0x27b49bc: clock / table[index], index from the object, set when
   it was made): adrp x8 at the first offset, ldr x8, [x8, #0xbc0] at the second. runtime/game_patches.c gives them a table whose
   every entry is the frame of the current mode, so effects made before a mode change keep the right speed. */
#define PSV_FRAME_SCALE_SITES {{0xb83d1c, 0xb001daa8, 0xb83d24}, {0xd96428, 0xd001ca08, 0xd96430}, {0x27b49b8, 0x9000f928, 0x27b49bc}}
#define PSV_FRAME_ADVANCE_SITES {{0x92dd20, 0xf001ed48, 0xf945e108}, {0x2b7b308, 0xb000dae8, 0xf945e108}}
#else
#error "build with -DPSV_GAME_V400 (the Makefile's VERSION)"
#endif
