// Common header of the native Switch host (multi-module).
#pragma once
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_profile.h"   // constants of the current game profile (generated from dm/games/<id>/game.tsv, chosen by the Makefile's GAME)

// Explicit host work does not occupy a guest core (Metal submission, or a blocked native codec lock). A contended host mutex must count as blocked: a signal's same-core handoff
// can otherwise wait forever for a peer that needs the signaller's native lock. Uncontended locks retain guest ordering; the counter nests inside existing host work.
extern __thread _Atomic int dm_host_work;
static inline int dm_host_mutex_lock(pthread_mutex_t *mutex) {
    int r = pthread_mutex_trylock(mutex);
    if (r != EBUSY) return r;
    dm_host_work++;
    r = pthread_mutex_lock(mutex);
    dm_host_work--;
    return r;
}
// Joining a native helper releases the guest core while that helper finishes; preserve any enclosing host-work scope.
static inline int dm_host_thread_join(pthread_t thread, void **result) {
    dm_host_work++;
    int r = pthread_join(thread, result);
    dm_host_work--;
    return r;
}

#define LOG(...) do { fprintf(stderr, "[dm] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(...) do { LOG("FATAL " __VA_ARGS__); abort(); } while (0)

typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { int64_t d_tag; union { uint64_t d_val, d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
enum { DT_INIT = 12, DT_FINI = 13, DT_RELA = 7, DT_RELASZ = 8, DT_JMPREL = 23, DT_PLTRELSZ = 2, DT_SYMTAB = 6, DT_STRTAB = 5,
       DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_RELRSZ = 35, DT_RELR = 36 };
enum { R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027 };
#define ELF64_ST_BIND(i) ((i) >> 4)            // symbol binding: 0 = STB_LOCAL, 2 = STB_WEAK
#define ELF64_ST_VISIBILITY(o) ((o) & 3)       // visibility: 0 = STV_DEFAULT
enum { STB_LOCAL = 0, STB_WEAK = 2, STV_DEFAULT = 0 };

// Run-time state of one NSO module
typedef struct {
    char name[16];
    uint8_t *base;             // Start address of the image (including the patch area) in the host address space
    size_t size;
    uint64_t text_end, ro_off, data_off, key_page_off, key_page_size;   // From the rewriter fixups header
    uint64_t fps_state_off;    // frame step state page (patch_nso.py FRAME_STEP: Tears of the Kingdom 1.4.3 and Breath of the Wild 1.9.0 main); 0 = none
    int64_t patch_off;         // patch area relative to the base; negative = before the base (image too large, see patch_nso.py FAR), and size does not include it
    uint64_t image_size, file_image_len, patch_len;   // only with the patch area before the base: whole image (with bss and key page), length of the image part of the file, patch area length
    char build_id[33];         // First 16 bytes of the NSO build ID (hex); empty for older build outputs
    unsigned key_idx[4];       // key page index of each thread slot (K18, KTLS, KTLR, KGATE)
    const Elf64_Dyn *dyn;
    const Elf64_Sym *symtab;
    const char *strtab;
    unsigned nsym;
    uint32_t *symh; unsigned symh_mask;   // Name hash table of defined symbols (open addressing, stores symbol indices; for a duplicated name only the smallest index is kept = the first hit of a linear scan)
    uint64_t init, init_array, init_arraysz;   // init = DT_INIT (the module's _init: registers the DSO and runs this module's init_array)
    uint64_t fini;             // DT_FINI (nn::ro::UnloadModule calls it)
    // NRO modules (nn::ro, loader.c "NRO modules"): mapped at start like the others, but they take part in symbol lookups only between LoadModule and UnloadModule
    int nro;
    _Atomic int loaded;        // the profile's modules always; NRO activation also synchronizes with borrowed stack backing
    uint8_t sha256[32];        // an NRO's SHA-256 (what the game's NRR registers; LoadModule finds the module by the image it passes)
    uint32_t nro_size;         // an NRO's file size (header +0x18), the bytes the hash covers
    uint8_t *pristine; size_t pristine_len;   // an NRO's initialized data as rewritten; reload restores this and zeroes its bss
    unsigned loads;            // how many times the game loaded it
} module_t;

extern module_t *g_mod;         // allocated once from the NSO list and NRO manifest; module addresses stay stable
extern int g_nmod;
// main's build ID (empty if none): code with fixed addresses in a game's module runs only on the build they were found in
static inline const char *dm_main_build(void) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return g_mod[i].build_id; return ""; }
// Four thread-local slots: K18 = the Switch's x18, KTLS = tpidr_el0, KTLR = tpidrro_el0 (ThreadLocalRegion), KGATE = host gate entry; the actual key numbers are written into the key page by the loader
extern pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;

void load_modules(const char *dir, const char **names, int n);
void run_module_inits(void);
module_t *nro_find(const uint8_t sha256[32]);   // the mapped NRO with this hash (loader.c)
int nro_refused(const uint8_t sha256[32]);      // an NRO the pack refused (an online SDK, nro.tsv "refused")
void nro_load(module_t *m);     // relocate an NRO against the loaded modules and bind the earlier modules' imports it defines (the caller runs its init)
void nro_unload(module_t *m);   // take an NRO out of the lookups and bind the imports that pointed into it again
int svc_nro_deactivate(module_t *m);   // mark inactive under the memory map lock, refusing while a stack borrows its data
void *module_sym(module_t *m, const char *name);
int dm_sdk_major(void);   // SDK major version from the sdk module's version string (loader.c); 0 = unknown
#define DM_SDK_OLD(major) ((major) > 0 && (major) <= 10)   // SDK 10 and older: NVN API 53, the shared-font address points at the TTF itself
int dm_main_imports(const char *prefix);   // main imports a symbol whose name starts with prefix (loader.c)
void *guest_sym(const char *name);
module_t *module_of(const void *p);
// Gate dispatch: rewritten svc / timer reads come in through the common entry, ret_addr = the return point of that gate block, regs points to the saved x0..x30
uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs);
// Observation gates (rewriter OBS tables in the profiles' patches.py, applied by patch_nso.py; numbering matches): read-only callbacks, called on a gate hit once attached
// IDs 19 / 20 belonged to the removed movie request-ring workaround. Old rewritten modules may still contain them; without callbacks those gates are harmless passes through.
enum { OBS_UE_MALLOC = 21,                                       // DRAGON QUEST VII Reimagined 1.2.0: Unreal's allocator object just made (hle_uemalloc.c)
       OBS_REGION_ASK = 22, OBS_REGION_GOT = 23, OBS_REGION_FREE = 24,   // the SDK's address regions, gates the rewriter places by symbol in every sdk (patch_nso.py "Address regions", hle_svc.c)
       OBS_STDALLOC_INIT = 25,                                         // StandardAllocator::Initialize's entry, the same way: the one gate whose callback writes a register (hle_svc.c)
       OBS_MAX = 26 };
// A gate number's bits from 8 up are a parameter for its callback (OBS_REGION_GOT: the register that holds the address). Callbacks read the registers; a write to x0–x29 takes
// effect when the gate returns (entry.S restores them from the frame), which only OBS_STDALLOC_INIT does
void gate_obs_set(int id, void (*fn)(uint64_t *regs, int param));
int gate_obs_present(int id);
uint64_t now_ns(void);
uint64_t guest_ns(void);              // Guest clock (hle_os.c; does not advance while frozen, guest_clock_freeze)
int dm_detclock_enabled(void);
void dm_guest_clock_advanced(void);
void guest_clock_freeze(int on);
void guest_clock_freeze_for(int reason, int on);   // Freeze by reason (1 background, 2 system keyboard); see hle_os.c
void guest_clock_read_wait(void);     // A guest read of the system counter: waits while the app is in the background (hle_os.c)
void dm_guest_frame_tick(void);        // Called on every present (hle_nvn.c): when DM_DETCLOCK is on, advances the guest clock by one fixed step
uint64_t dm_nvn_frame_count(void);
uint64_t dm_nvn_sub_cpu_ns(void);     // Cumulative CPU nanoseconds of submit (excluding present)
void dm_backtrace(uint64_t fp);
void crash_watch_progress(uint32_t tick, const char *stage);
void crash_watch_frame(void);
void crash_watch_pregame_done(void);
uint64_t crash_watch_progress_count(void);
void crash_dump_now(const char *why);   // Dump state on demand (threads + graphics wait points), without exiting
// Diagnostic switches: read once from environment variables when main starts (getenv takes the libc global environment lock, and on a hot path it would make all guest threads queue up)
extern uint64_t g_t0_ns;   // Host start time (now_ns), used for relative times in the log
extern struct dm_flags { int thr, svc, fs, dbg, imports, bootprof; } g_flag;
void *objmap_get(const void *guest); void objmap_put(const void *guest, void *host); void *objmap_del(const void *guest);
void hle_signal_system_event(void *g);   // hle_thread.c: signal a service-layer SystemEvent (any thread)
void hle_bind_system_event(void *g, int autoclear);   // On a guest thread, bind a guest SystemEvent to a kernel event
void hle_clear_system_event(void *g);    // hle_thread.c: clear a bound service-layer SystemEvent (any thread)
int hle_signal_bound_system_event(void *g);   // hle_thread.c: signal a service-layer SystemEvent only if it is bound (any thread); 0 when it is not
uint64_t hle_system_event_binding(void *g);   // Opaque lifetime ID of a binding, 0 if unbound; a rebind gets a new ID even if its address and handle are reused
int hle_signal_system_event_binding(uint64_t binding);   // Signal only this binding lifetime (any thread); 0 after its write handle closes
// Page fault hook / hook before the host writes guest memory (crash.c; used for write-protect dirty page tracking)
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len));
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why));   // Resource count callback (registered by the graphics layer): called every 250 ticks, when the loading screen has been dismissed, and on exit when the record is full
void (*dm_res_report_cb(void))(uint32_t tick, const char *why);   // Get the registered resource count callback (null if not registered)
void dm_guest_write_prepare(void *addr, size_t len);
// Graphics lifecycle (hle_nvn.c): on=1 suspends (afterwards submit / present block, and in-flight GPU work is drained before returning), on=0 resumes; called by the iOS front end when entering and leaving the background
void dm_gfx_suspend(int on);
void dm_gfx_enter(void); void dm_gfx_leave(void);   // hle_nvn.c: the gate dm_gfx_suspend closes, for graphics layers other than NVN (dm/gfx/vk)
void dm_gfx_shot_at(long k, const char *path);
void dm_shot_written(const char *path, int ok);   // The graphics side finished writing a screenshot to disk (diag.c: DM_TICK_HOLD=E:SHOT unfreezes on this)
void dm_host_shutdown(const char *why);   // Graceful exit (crash.c): stop background threads and audio, drain graphics, then _exit(0); used by SIGTERM and the iOS App termination callback   // graphics side: screenshot of the k-th frame after the current presented frame (nvn_mtl.m)
