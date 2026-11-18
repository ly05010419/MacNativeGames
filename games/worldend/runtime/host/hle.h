// HLE common: register host implementations by Switch mangled name.
// Note the differences between Apple AAPCS64 and Switch: Apple requires the caller to extend <32-bit arguments (bool/char/short), Switch code does not extend ⇒
// such parameters are always declared as int/unsigned and truncated by ourselves. Variadic arguments go on the stack on Apple and in registers on Switch ⇒ variadic functions cannot use C's "..." directly.
#pragma once
#include "dm.h"
typedef struct { const char *name; void *fn; } hle_ent;
#define HLE(mangled, ret, cname, ...)                                                             \
    static ret cname(__VA_ARGS__);                                                                \
    __attribute__((used, section("__DATA,swhle"))) static const hle_ent hle_ent_##cname = {mangled, (void *)cname}; \
    static ret cname(__VA_ARGS__)

typedef uint32_t Result;   // nn::Result: w0, 0 = success
#define RES_OK 0u
#define RES_FAIL 0x2A2u      // Generic failure (cut features such as networking uniformly return this)

// Guest address → host object side table
void *objmap_get(const void *guest);
void objmap_put(const void *guest, void *host);
void *objmap_del(const void *guest);
uint64_t now_ns(void);
// This thread is doing the host's own heavy work (NVN submit / present: encoding Metal commands; on hardware that is the GPU's business and does not occupy a guest core). The same-core preemption approximation (hle_thread.c) does not count it as "running"
extern __thread _Atomic int dm_host_work;
uint64_t guest_ns(void);              // Guest clock (hle_os.c; does not advance while frozen, guest_clock_freeze)
int dm_detclock_enabled(void);
void dm_guest_clock_advanced(void);
uint64_t dm_guest_frame(void);        // Incremented each time the host presents a frame; unaffected by machine load
void guest_clock_freeze(int on);
void guest_clock_freeze_for(int reason, int on);   // Freeze by reason (1 background, 2 system keyboard); see hle_os.c
void guest_clock_read_wait(void);     // A guest read of the system counter: waits while the app is in the background (hle_os.c)

// Forced takeover: even if the SDK module has a definition, all references to that symbol at load time (JUMP_SLOT/GLOB_DAT/ABS64) are resolved to the host implementation.
// Used only for the service-layer nn:: (service clients of fs/oe/aoc/pctl...); the SDK's libc++ and nn::os internals do not go into this table.
// Every entry must state the reason for the takeover; an entry in the table that no module references makes loading report a failure and abort (a wrong list must fail loudly).
typedef struct { const char *name; void *fn; const char *why; } hle_force_ent;
#define HLE_FORCE(mangled, why, ret, cname, ...)                                                   \
    static ret cname(__VA_ARGS__);                                                                \
    __attribute__((used, section("__DATA,swforce"))) static const hle_force_ent hle_force_##cname = {mangled, (void *)cname, why}; \
    static ret cname(__VA_ARGS__)
// Same as above, but the implementation runs on this thread's big host stack (hle_bigstack.c): what is registered is a stub generated at compile time (the implementation's address goes into x16, then a jump to dm_bigstack_enter).
// For take-overs with large stack use (the file system sizes its buffers by PATH_MAX): a game thread's stack can be as small as 8 KB. Only register arguments are carried over (<= 8 integer, <= 8 floating point); not for variadic functions
#define HLE_FORCE_BIG(mangled, why, ret, cname, ...)                                               \
    static ret cname(__VA_ARGS__) __attribute__((used));                                          \
    extern const char cname##_bs[];                                                               \
    __asm__(".text\n.p2align 2\n.globl _" #cname "_bs\n.private_extern _" #cname "_bs\n_" #cname "_bs:\n" \
            "    adrp x16, _" #cname "@PAGE\n    add x16, x16, _" #cname "@PAGEOFF\n    b _dm_bigstack_enter\n"); \
    __attribute__((used, section("__DATA,swforce"))) static const hle_force_ent hle_force_##cname = {mangled, (void *)(uintptr_t)cname##_bs, why}; \
    static ret cname(__VA_ARGS__)

// Operation mode (0 handheld / 1 docked): owned by hle_oe.c, and the hid controller style follows it; the front end can switch it at runtime
int dm_opmode(void);
void dm_set_opmode(int m);          // On change, push notification 30 / 31 according to the switches the game has turned on
// Focus (1 foreground / 2 focus lost / 3 background): called by the front end on lifecycle changes; a change pushes notification 15
void dm_focus_set(int st);
// Audio pause / resume following the lifecycle (called by the front end on going to background / returning to foreground)
extern uint8_t *dm_fps_state;   // frame step state page (loader.c install_fps_state): +0 float target fps, +4 movie flag, +0x10 float render scale, +0x18 double game seconds stepped; NULL when the build has none
void dm_audio_suspend(int on);
uint64_t dm_audio_render_calls(void);   // Cumulative sound card callback count (the silent sink is not counted)
void dm_audio_reset(void);   // iOS media services reset: recreate the AudioUnits (hle_audio.c)
