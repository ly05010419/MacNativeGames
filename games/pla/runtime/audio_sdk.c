/* PLA: the SDK-level AudioOut API, served by the host. The game imports exactly this family and no renderer at all
   (main's imports: OpenDefaultAudioOut, AppendAudioOutBuffer, SetAudioOutBufferInfo, GetAudioOutBufferDataPointer,
   GetReleasedAudioOutBuffer, Start/Stop/Close/GetAudioOutState, GetAudioOutChannelCount, InitializeAudioOutParameter,
   plus the device name / channel count / notification queries), so the host answers the API itself and plays through the
   same core as the audout:u service (audio_out.c), the way nxnative's `hle_audio.c` does for its games. Serving it
   through the audout:u IPC service alone was not enough: PLA never opened that service.
   Buffer layout is the public one (switchbrew AudioOutBuffer, 0x28 bytes): next, buffer, capacity, data_size, data_offset. */
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <libkern/OSCacheControl.h>   /* sys_icache_invalidate */
#include <sys/mman.h>
#include <pthread.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

/* Every guest pointer read from a hook must be range checked: the very first PostEvent can run before the
   engine object exists, and a bogus pointer faults inside the guest (SIGBUS on a read-only page). */
#define AU_PTR_OK(p) ((uint64_t)(p) >= 0x100000000ULL && (uint64_t)(p) < 0x40000000000ULL && (((uint64_t)(p) & 7) == 0))


typedef struct { void *next, *buffer; uint64_t capacity, data_size, data_offset; } AuBuffer;
_Static_assert(sizeof(AuBuffer) == 0x28, "AudioOutBuffer");

typedef struct { uint32_t sample_rate, channel_count, pcm_format, state; } AuOutHead;   /* the guest AudioOut, as the host fills it */

void *au_core_open(uint32_t rate, uint32_t channels);
uint64_t au_core_released_wait(void *p, unsigned ms);
uint32_t au_core_channels(void *p);
uint32_t au_core_state(void *p);
void au_core_start_stop(void *p, int start);
int au_core_append(void *p, uint64_t tag, const void *pcm, uint64_t bytes);
uint64_t au_core_released(void *p);
void au_core_close(void *p);

enum { AU_OPEN_MAX = 64 };

/* PLA_AUDIO_CYCLE_LOG=<from>-<to>: log every SDK-AudioOut call in that ordinal window, uncapped, to read one
   full sink lifecycle exactly (the TRACE counters sample at 128 and hide the shape of a cycle). */
static _Atomic unsigned g_cyclen;
static void cycle_log(const char *what, uint64_t a, uint64_t b) {
    const char *e = getenv("PLA_AUDIO_CYCLE_LOG");
    if (!e) return;
    unsigned i = atomic_fetch_add(&g_cyclen, 1);
    unsigned from = 0, to = 0;
    sscanf(e, "%u-%u", &from, &to);
    if (!to || i < from || i > to) return;
    fprintf(stderr, "[cycle] %u %s(%#llx, %#llx)\n", i, what, (unsigned long long)a, (unsigned long long)b);
}

/* PLA_AUDIO_TRACE=1 (diagnostic): log the AudioOut-family traffic (rate-limited) with the guest call site, to see
   the sink's open/append/released/start cycle when the engine tears down and reopens its output. */
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);   /* loader.c */

static uint64_t au_modoff(void *pc) {
    const char *m = "?"; uint64_t o = 0;
    runtime_module_offset((uint64_t)(uintptr_t)pc, &m, &o);
    return o;
}
static void au_trace(const char *what, uint64_t a, uint64_t b) {
    (void)a; (void)b;
    static _Atomic unsigned n[8];
    int k = what[0] & 7;
    unsigned i = atomic_fetch_add(&n[k], 1);
    if (i >= 8 && i % 128) return;
    (void)a; (void)b;
    fprintf(stderr, "[totkaudio] %s #%u\n", what, i);
}

static struct { void *guest; void *core; } g_au[AU_OPEN_MAX];
static unsigned g_nau;

static void *au_for(const void *guest) {
    for (unsigned i = 0; i < g_nau; i++) if (g_au[i].guest == guest) return g_au[i].core;
    return NULL;
}

static uint32_t au_open(void *guest, const uint32_t *param) {
    if (!guest) return 0x40299;
    /* The sink object is re-initialised in place (open/close cycles reuse the same pointer). Close any stale
       registration first so the guest can never address a dead core through an old slot. */
    for (unsigned i = 0; i < g_nau; i++) if (g_au[i].guest == guest) {
        au_core_close(g_au[i].core);
        g_au[i] = g_au[--g_nau];
        break;
    }
    uint32_t rate = param ? param[0] : 0, channels = param ? param[1] : 0;
    if (!rate) rate = 48000;
    if (!channels) channels = 2;
    void *core = au_core_open(rate, channels);
    if (!core) return 0x40299;
    if (g_nau < AU_OPEN_MAX) { g_au[g_nau].guest = guest; g_au[g_nau].core = core; g_nau++; }
    AuOutHead h = {rate, channels, 2 /* PCM16 */, 0 /* stopped */};
    memcpy(guest, &h, sizeof h);
    return 0;
}
static uint32_t au_open_default(void *guest, const void *param) {
    const uint32_t *p = param;
    cycle_log("Open", (uint64_t)(uintptr_t)guest, (p ? p[0] : 0) | ((uint64_t)(p ? p[1] : 0) << 32));
    if (getenv("PLA_AUDIO_TRACE")) au_trace("OpenDefault", (uint64_t)(uintptr_t)guest, (p ? p[0] : 0) | ((uint64_t)(p ? p[1] : 0) << 32));
    if (getenv("PLA_AUDIO_CALLER")) {
        const char *m = "?"; uint64_t off = 0;
        runtime_module_offset((uint64_t)(uintptr_t)__builtin_return_address(0), &m, &off);
        fprintf(stderr, "[totkaudio] OpenDefaultAudioOut <- %s+0x%llx\n", m, (unsigned long long)off);
    }
    uint32_t r = au_open(guest, param);
    cycle_log("OpenRet", r, 0);
    return r;
}
static uint32_t au_open_named(void *guest, const char *name, const void *param, void *event) {
    if (getenv("PLA_AUDIO_TRACE")) fprintf(stderr, "[totkaudio] OpenAudioOut(%p) name=%s\n", guest, name ? name : "(default)");
    (void)event;
    return au_open(guest, param);
}
static uint32_t au_append(void *guest, AuBuffer *b) {
    cycle_log("Append", (uint64_t)(uintptr_t)guest, b ? b->data_size : 0);
    void *core = au_for(guest);
    if (!core || !b) return 0x40299;
    const uint8_t *pcm = b->buffer ? (const uint8_t *)b->buffer + b->data_offset : NULL;
    /* Append is the Auto form: the sink prepared the buffer with SetAudioOutBufferInfo (data_size = 0, the payload
       comes later) and hands over the whole buffer — the SDK's own client, which this override replaces, sets
       data_size = capacity for it. Without this every append answers "queue full" (the core treats 0 bytes as
       refused), the sink starves and the game tears it down and reopens it forever. */
    uint64_t bytes = b->data_size;
    if (!bytes) bytes = b->capacity;
    if (bytes > b->capacity) bytes = b->capacity;
    /* PLA_AUDIO_PCM_DUMP=<path> (diagnostic): append the played PCM (16-bit, the output's layout) to a file,
       to tell real music from silence or noise without listening (RMS per second, spectral flatness). */
    { const char *dp = getenv("PLA_AUDIO_PCM_DUMP");
      if (dp && pcm) { static FILE *f; static int tried; if (!tried) { tried = 1; f = fopen(dp, "wb"); }
                       if (f) fwrite(pcm, 1, bytes, f); } }
    if (getenv("PLA_AUDIO_BUF_PROBE")) {
        static int n;
        if (n++ < 8) {
            unsigned nz_base = 0, nz_pcm = 0;
            const uint8_t *base = (const uint8_t *)b->buffer;
            for (unsigned i = 0; base && i < 4096 && i < b->capacity; i++) if (base[i]) nz_base++;
            for (unsigned i = 0; pcm && i < bytes && i < 8192; i++) if (pcm[i]) nz_pcm++;
            fprintf(stderr, "[aubuf] Append b=%p buffer=%p capacity=%llu data_size=%llu data_offset=%llu bytes=%llu"
                            " nz(base,cap)=%u nz(pcm,bytes)=%u caller main+0x%llx\n",
                    (void *)b, (void *)b->buffer, (unsigned long long)b->capacity, (unsigned long long)b->data_size,
                    (unsigned long long)b->data_offset, (unsigned long long)bytes, nz_base, nz_pcm,
                    (unsigned long long)au_modoff(__builtin_return_address(0)));
        }
    }
    int r = au_core_append(core, (uint64_t)(uintptr_t)b, pcm, bytes);
    cycle_log("AppendRet", r, bytes);
    /* Return convention, A/B switchable while we pin down which one this SDK's client expects:
         default: 0 = accepted, 1 = queue full   (Result-like)
         PLA_APPEND_BOOL=1: 1 = accepted, 0 = queue full   (bool, as nxnative's host answers) */
    if (getenv("PLA_APPEND_BOOL")) { static int on = -1; if (on < 0) on = 1; return r ? 1 : 0; }
    if (!r) return 1;
    return 0;
}
static void au_start(void *guest) { cycle_log("Start", (uint64_t)(uintptr_t)guest, 0); if (getenv("PLA_AUDIO_TRACE")) au_trace("Start", (uint64_t)(uintptr_t)guest, 0); void *c = au_for(guest); if (c) au_core_start_stop(c, 1); }
static void au_stop(void *guest) { cycle_log("Stop", (uint64_t)(uintptr_t)guest, 0); if (getenv("PLA_AUDIO_TRACE")) au_trace("Stop", (uint64_t)(uintptr_t)guest, 0); void *c = au_for(guest); if (c) au_core_start_stop(c, 0); }
static void au_close(void *guest) {
    cycle_log("Close", (uint64_t)(uintptr_t)guest, 0);
    if (getenv("PLA_AUDIO_TRACE")) au_trace("Close", (uint64_t)(uintptr_t)guest, 0);
    for (unsigned i = 0; i < g_nau; i++) if (g_au[i].guest == guest) {
        au_core_close(g_au[i].core);
        g_au[i] = g_au[--g_nau]; return; }
}
static uint32_t au_state(const void *guest) { cycle_log("State", (uint64_t)(uintptr_t)guest, 0); void *c = au_for(guest); uint32_t r = c ? au_core_state(c) : 1; cycle_log("StateRet", (uint64_t)(uintptr_t)guest, r); return r; }   /* 1 = stopped, as a closed device answers */
static uint32_t au_channels(const void *guest) { void *c = au_for(guest); uint32_t r = c ? au_core_channels(c) : 0; cycle_log("ChannelsRet", r, 0); return r; }
static void au_set_buffer_info(AuBuffer *b, void *buffer, uint64_t capacity, uint64_t offset) {
    cycle_log("SetBufInfo", (uint64_t)(uintptr_t)b, capacity | (offset << 32));
    if (getenv("PLA_AUDIO_BUF_PROBE")) {
        static int n;
        if (n++ < 6) fprintf(stderr, "[aubuf] SetBufInfo b=%p buffer=%p capacity=%llu arg4=%llu caller main+0x%llx\n",
                             (void *)b, buffer, (unsigned long long)capacity, (unsigned long long)offset,
                             (unsigned long long)au_modoff(__builtin_return_address(0)));
    }
    /* The SDK's own SetAudioOutBufferInfo stores the 4th argument in data_size (+0x18) and never touches a
       data_offset field (sdk.elf 0x3ac70: "str x3,[x0,#0x18]"), and GetAudioOutBufferDataPointer is just
       "ldr x0,[x0,#8]" (sdk.elf 0x3ac80: buffer + 0).  This adapter used to treat arg4 as an offset, so the
       guest filled buffer+4096 — outside the 4096-byte allocation the sink makes — and Append read the same
       out-of-bounds window.  Honour the SDK's semantics instead. */
    if (!b) return; b->buffer = buffer; b->capacity = capacity; b->data_size = offset; b->data_offset = 0;
}
/* The payload pointer: sample_buffer + data_offset — the sink writes its PCM there (it set the offset with
   SetAudioOutBufferInfo) and Append reads the same window back. */
static void *au_buffer_pointer(const AuBuffer *b) {
    cycle_log("DataPtr", (uint64_t)(uintptr_t)b, 0);
    return b && b->buffer ? (uint8_t *)b->buffer : NULL;   /* the SDK returns sample_buffer itself */
}
static AuBuffer *au_released(void *guest) {
    cycle_log("Released", (uint64_t)(uintptr_t)guest, 0);
    /* Non-blocking, poll-only as on hardware: the engine idles its output at silent scenes and the game probes
       it with short cycles; any wait here lands on the caller's thread (50 ms each ran the game at 265 ms/frame).
       Releases are picked up on the game's own cadence — buffers come back every ~21 ms while anything plays. */
    void *c = au_for(guest); uint64_t tag = c ? au_core_released_wait(c, 60) : 0;
    cycle_log(tag ? "ReleasedGot" : "ReleasedEmpty", (uint64_t)(uintptr_t)tag, 0);
    if (getenv("PLA_AUDIO_TRACE")) au_trace(tag ? "Released" : "Released-none", (uint64_t)(uintptr_t)guest, tag);
    return (AuBuffer *)(uintptr_t)tag;
}

/* PLA_AUDIO_SINK_PROBE=1 (diagnostic): does the game ever construct its audio sink? The sink's init method is at
   main+0x2e35b14 (main calls InitializeAudioOutParameter -> OpenDefaultAudioOut -> Append -> Start there); nothing
   calls it directly, so a probe at its entry is the only way to see whether the game reaches it. The stub logs and
   then runs the replaced prologue (three instructions) before continuing at main+0x2e35b30. */
extern void runtime_audio_chain_probe_install(void);
void sink_probe_note(void);
static void *sink_probe_cont;
__attribute__((naked)) static void sink_probe_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "stp x4, x5, [sp, #-16]!\n"
        "stp x6, x7, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"                      /* the caller's return address; bl below overwrites x30 */
        "bl _sink_probe_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x6, x7, [sp], #16\n"
        "ldp x4, x5, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "str x27, [sp, #-0x60]!\n"                    /* the prologue at main+0x2e35b14..0x2e35b2c, replayed whole */
        "stp x26, x25, [sp, #0x10]\n"
        "stp x24, x23, [sp, #0x20]\n"
        "stp x22, x21, [sp, #0x30]\n"
        "stp x20, x19, [sp, #0x40]\n"
        "stp x29, x30, [sp, #0x50]\n"
        "add x29, sp, #0x50\n"
        "adrp x16, _sink_probe_cont@PAGE\n"
        "ldr x16, [x16, _sink_probe_cont@PAGEOFF]\n"
        "br x16\n");
}
void sink_probe_note(void) {
    static int n;
    if (n++ < 16) { fprintf(stderr, "[audprobe] audio sink init reached (#%d)\n", n); fflush(stderr); }
}
static void emit_adrp_br(uint32_t *at, uintptr_t pc, const void *target) {
    uintptr_t t = (uintptr_t)target;
    int64_t imm = ((int64_t)(t & ~0xfffULL) - (int64_t)(pc & ~0xfffULL)) >> 12;
    at[0] = 0x90000010u | ((uint32_t)(imm & 3) << 29) | (((uint32_t)(imm >> 2) & 0x7ffff) << 5);
    at[1] = 0x91000210u | ((uint32_t)(t & 0xfff) << 10);
    at[2] = 0xd61f0200u;
}
/* PLA_AUDIO_SR_PROBE=1 (diagnostic): log every call of Wwise's suspend/resume toggle (main+0x2dc9278, w0: 1 suspend,
   0 resume) with its arguments and the guest call site it came from. The engine is suspended at boot; this shows
   whether anything ever resumes it, and which of the three call sites fired. */
uint8_t *sr_probe_base;                                   /* main's base, for caller offsets */
void sink_probe_sr_note(uint64_t a0, uint64_t a1, uint64_t caller);
static void *sr_probe_cont;
__attribute__((naked)) static void sr_probe_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"                   /* the guest caller's return address; bl below overwrites x30 */
        "mov x2, x30\n"
        "bl _sink_probe_sr_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "str x21, [sp, #-0x30]!\n"                   /* the replaced prologue: 0x2dc9278..0x2dc9280 */
        "stp x20, x19, [sp, #0x10]\n"
        "stp x29, x30, [sp, #0x20]\n"
        "adrp x16, _sr_probe_cont@PAGE\n"
        "ldr x16, [x16, _sr_probe_cont@PAGEOFF]\n"
        "br x16\n");
}
void sink_probe_sr_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n;
    if (n++ < 32) {
        fprintf(stderr, "[audprobe] suspend/resume toggle w0=%llu w1=%llu caller main+0x%llx\n",
                (unsigned long long)a0, (unsigned long long)a1,
                (unsigned long long)(caller ? caller - (uint64_t)sr_probe_base : 0));
        fflush(stderr);
    }
}
/* Note-trampolines: at each probed entry the host patches adrp/add/br to one of these stubs. The stub saves
   x0-x7 (the method's arguments), calls the note, restores them, replays the entry's original prologue words
   (frame pushes only; PC-relative words are never chosen), and continues past them. */
void dev_note_log(const char *name);
__attribute__((naked)) static void dev_m1_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "stp x4, x5, [sp, #-16]!\n"
        "stp x6, x7, [sp, #-16]!\n"
        "bl _dev_note_log_args\n"
        "ldp x6, x7, [sp], #16\n"
        "ldp x4, x5, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "str x23, [sp, #-0x40]!\n"
        "stp x22, x21, [sp, #0x10]\n"
        "stp x20, x19, [sp, #0x20]\n"
        "adrp x16, _dev_m1_cont@PAGE\n"
        "ldr x16, [x16, _dev_m1_cont@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void dev_m3_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "stp x4, x5, [sp, #-16]!\n"
        "stp x6, x7, [sp, #-16]!\n"
        "bl _dev_note_log_args\n"
        "ldp x6, x7, [sp], #16\n"
        "ldp x4, x5, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "stp x29, x30, [sp, #-0x10]!\n"
        "add x29, sp, #0\n"
        "adrp x16, _dev_m3_cont@PAGE\n"
        "ldr x16, [x16, _dev_m3_cont@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void dev_fac_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "stp x4, x5, [sp, #-16]!\n"
        "stp x6, x7, [sp, #-16]!\n"
        "bl _dev_note_log_args\n"
        "ldp x6, x7, [sp], #16\n"
        "ldp x4, x5, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "stp x22, x21, [sp, #-0x20]!\n"
        "stp x29, x30, [sp, #0x10]\n"
        "add x29, sp, #0x10\n"
        "adrp x16, _dev_fac_cont@PAGE\n"
        "ldr x16, [x16, _dev_fac_cont@PAGEOFF]\n"
        "br x16\n");
}
/* The stubs call dev_note_log_args with the name in x0, which tail-jumps to dev_note_log. */
__attribute__((naked, used)) static void dev_note_log_args(void) {
    __asm__ volatile(
        "adrp x16, _dev_note_log@PAGE\n"
        "ldr x16, [x16, _dev_note_log@PAGEOFF]\n"
        "br x16\n");
}
void dev_note_log(const char *name) {
    static int n;
    if (n++ < 120) { fprintf(stderr, "[audprobe] hit %s (#%d)\n", name, n); fflush(stderr); }
}
static void *dev_m1_cont, *dev_m3_cont, *dev_fac_cont;
static void install_note_probe(uint8_t *fn, const char *name, const void *stub, int nwords);
static void install_note_probe(uint8_t *fn, const char *name, const void *stub, int nwords) {
    (void)name; (void)nwords;
    uint32_t patch[3] = {0, 0, 0};
    emit_adrp_br(patch, (uintptr_t)fn, stub);
    memcpy(fn, patch, sizeof patch);
    sys_icache_invalidate(fn, sizeof patch);
    fprintf(stderr, "[audprobe] %s probe installed at %p\n", name, (void *)fn);
}
__attribute__((naked)) static void dev_m1_stub(void);
__attribute__((naked)) static void dev_m3_stub(void);
__attribute__((naked)) static void dev_fac_stub(void);
void runtime_audio_sink_probe_install(void) {
    if (!getenv("PLA_AUDIO_SINK_PROBE")) { runtime_audio_chain_probe_install(); return; }
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) { fprintf(stderr, "[audprobe] no nnMain\n"); return; }
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;          /* nnMain's offset in this build (pinned) */
    sr_probe_base = base;
    uint8_t *ctor = base + 0x2e35b14ULL;
    uint32_t expect[3]; memcpy(expect, ctor, sizeof expect);
    if (expect[0] != 0xf81a0ffbu || expect[1] != 0xa90167fau || expect[2] != 0xa9025ff8u) {
        fprintf(stderr, "[audprobe] audio sink prologue differs (%#x %#x %#x): probe not installed\n", expect[0], expect[1], expect[2]); return; }
    sink_probe_cont = base + 0x2e35b30ULL;
    uint32_t patch[3] = {0, 0, 0};
    emit_adrp_br(patch, (uintptr_t)ctor, (const void *)sink_probe_stub);
    memcpy(ctor, patch, sizeof patch);
    sys_icache_invalidate(ctor, sizeof patch);
    fprintf(stderr, "[audprobe] audio sink probe installed at %p (cont %p)\n", (void *)ctor, sink_probe_cont);
    /* PLA_AUDIO_FORCE_RESUME=1 (diagnostic): the sound manager's idle timeout calls Wwise's suspend/resume toggle with
       w0 = 1 (suspend); patch that call site (main+0x2d7e990, `mov w0, #1`) to pass 0 (resume) instead, to see whether a
       resume is what creates the audio device and starts the bank loads. */
    if (getenv("PLA_AUDIO_FORCE_RESUME")) {
        uint8_t *site = base + 0x2d7e990ULL; uint32_t w; memcpy(&w, site, 4);
        if (w == 0x52800020u) { uint32_t m = 0x52800000u; memcpy(site, &m, 4); sys_icache_invalidate(site, 4); fprintf(stderr, "[audprobe] suspend call site -> resume (w0 = 0)\n"); }
        else fprintf(stderr, "[audprobe] suspend call site differs (%#x)\n", w);
    }
    /* PLA_AUDIO_DEV_PROBE=1 (diagnostic): trampolines on the audio device object's methods that the manager tick
       drives — dev.m1 (vtable 0x41ad250 = 0x2e334e0) and dev.m3.getkind (0x2e33638, called every tick while the
       device's ready flag is set) — plus the sink factory (0x2e35840, the only creator of the output sink). The stubs
       replay the site's original prologue, so each site is verified first. */
    if (getenv("PLA_AUDIO_DEV_PROBE")) {
        static const struct { uint64_t off; const char *name; uint32_t w[3]; int nwords; } devs[] = {
            {0x2e334e0, "dev.m1",          {0xf81d0ff5u, 0xa9014ff4u, 0xa9027bfdu}, 3},
            {0x2e33638, "dev.m3.getkind",  {0xa9bf7bfdu, 0x910003fdu, 0},           2},   /* word 3 is adrp (PC-relative): not replayed */
            {0x2e35840, "sink.factory",    {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, 3},
        };
        for (unsigned i = 0; i < sizeof devs / sizeof devs[0]; i++) {
            uint8_t *fn = base + devs[i].off;
            uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
            if (memcmp(ex, devs[i].w, (size_t)devs[i].nwords * 4)) { fprintf(stderr, "[audprobe] %s prologue differs (%#x %#x %#x), skipped\n", devs[i].name, ex[0], ex[1], ex[2]); continue; }
            void *stub = i == 0 ? (void *)dev_m1_stub : i == 1 ? (void *)dev_m3_stub : (void *)dev_fac_stub;
            void **cont = i == 0 ? &dev_m1_cont : i == 1 ? &dev_m3_cont : &dev_fac_cont;
            *cont = fn + devs[i].nwords * 4;
            install_note_probe(fn, devs[i].name, stub, devs[i].nwords * 4);
        }
    }
    if (getenv("PLA_AUDIO_SR_PROBE")) {
        uint8_t *sr = base + 0x2dc9278ULL;
        uint32_t ex[3]; memcpy(ex, sr, sizeof ex);
        if (ex[0] == 0xf81d0ff5u && ex[1] == 0xa9014ff4u && ex[2] == 0xa9027bfdu) {
            sr_probe_cont = base + 0x2dc9284ULL;   /* after the 3 pushes; `add x29, sp, #0x20` runs natively */
            uint32_t pt[3] = {0, 0, 0};
            emit_adrp_br(pt, (uintptr_t)sr, (const void *)sr_probe_stub);
            memcpy(sr, pt, sizeof pt);
            sys_icache_invalidate(sr, sizeof pt);
            fprintf(stderr, "[audprobe] suspend/resume probe installed at %p\n", (void *)sr);
        } else fprintf(stderr, "[audprobe] suspend/resume prologue differs (%#x %#x %#x)\n", ex[0], ex[1], ex[2]);
    }
    runtime_audio_chain_probe_install();
}

/* PLA_AUDIO_CHAIN_PROBE=1 (diagnostic): the game's sound init wrapper (main+0x2e33298) runs a precondition chain
   (main+0x2e137e8): setup tables, then gates — config byte main+0x432d1c8 bit0, pointer main+0x430a098, the sound
   manager create (main+0x2e332ec), steps at main+0x2dac734/0x2db278c/0x2df8ca8 — and only the full path reaches the
   chain's last step (main+0x2dc903c, the one that also creates the "AK Suspended" thread). The wrapper then calls the
   audio device setup (main+0x2e33c9c) only when the chain result != 2; a skipped step returns 2 and no device is ever
   constructed. Entry probes on the wrapper, the manager create, the last chain step and the device setup, plus a
   snapshot of the two gate globals, show which condition stops the device in one boot. */
static uint8_t *chain_probe_base;
static uint8_t *g_site_modbase;
uint8_t *chain_probe_sndinit_g;                    /* main+0x430a000, replayed for the wrapper's adrp */
uint8_t *chain_probe_g_df8ca8;                     /* main+0x2df8ca8, replayed for the chain tail's bl */
uint8_t *chain_probe_g_endstep;                    /* main+0x2dc903c, the patched-over tail-call target */
void chain_probe_sndinit_note(uint64_t caller);
void chain_probe_step_note(int tag, uint64_t caller, uint64_t a1);
static void *chain_probe_cont_sndinit, *chain_probe_cont_mgr, *chain_probe_cont_end, *chain_probe_cont_dev;
static void *chain_probe_cont_tail, *chain_probe_cont_devq, *chain_probe_cont_devqr, *chain_probe_cont_start;

/* Common shape of every entry stub below: save x0/x1 and the guest caller's x30 (bl clobbers it), note(tag,
   caller, arg), restore, replay the three replaced instructions exactly (frame pushes re-save the original
   x30 where the original code did), then continue at +12. Sites whose replaced window does not save x30
   rely on the function saving it shortly after the continuation, as checked when each probe was written. */
__attribute__((naked)) static void chain_probe_sndinit_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x0, #0\n"
        "mov x1, x30\n"
        "bl _chain_probe_step_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "stp x29, x30, [sp, #-16]!\n"                /* the replaced prologue: 0x2e33298..0x2e332a0 */
        "mov x29, sp\n"
        "adrp x16, _chain_probe_sndinit_g@PAGE\n"    /* replay of adrp x8, #0x430a000 (same value in x8) */
        "ldr x8, [x16, _chain_probe_sndinit_g@PAGEOFF]\n"
        "adrp x16, _chain_probe_cont_sndinit@PAGE\n"
        "ldr x16, [x16, _chain_probe_cont_sndinit@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void chain_probe_mgr_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x0, #1\n"
        "mov x1, x30\n"
        "bl _chain_probe_step_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "str x19, [sp, #-32]!\n"                     /* the replaced prologue: 0x2e332ec..0x2e332f4 */
        "stp x29, x30, [sp, #16]\n"
        "add x29, sp, #16\n"
        "adrp x16, _chain_probe_cont_mgr@PAGE\n"
        "ldr x16, [x16, _chain_probe_cont_mgr@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void chain_probe_tail_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x0, #7\n"
        "mov x1, x30\n"                              /* the chain runner's caller (= the wrapper's bl site) */
        "bl _chain_probe_step_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "adrp x16, _chain_probe_g_df8ca8@PAGE\n"     /* replay: bl 0x2df8ca8 (step 8), then the epilogue */
        "ldr x16, [x16, _chain_probe_g_df8ca8@PAGEOFF]\n"
        "blr x16\n"
        "ldp x29, x30, [sp], #16\n"                  /* replay: ldp x29,x30,[sp],#0x10 — restores the guest caller's x30 */
        "adrp x16, _chain_probe_g_endstep@PAGE\n"    /* replay: b 0x2dc903c (the tail call, x30 now correct) */
        "ldr x16, [x16, _chain_probe_g_endstep@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void chain_probe_end_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x0, #2\n"
        "mov x1, x30\n"                              /* 0x2e1384c = the chain's tail call; anything else is another caller */
        "bl _chain_probe_step_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #48\n"                          /* the replaced prologue: 0x2dc903c..0x2dc9044 */
        "stp x20, x19, [sp, #16]\n"
        "stp x29, x30, [sp, #32]\n"
        "adrp x16, _chain_probe_cont_end@PAGE\n"
        "ldr x16, [x16, _chain_probe_cont_end@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void chain_probe_dev_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x2, x0\n"                               /* the setup's argument (main+0x430b2d8) */
        "mov x0, #3\n"
        "mov x1, x30\n"
        "bl _chain_probe_step_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "stp x26, x25, [sp, #-80]!\n"                /* the replaced prologue: 0x2e33c9c..0x2e33ca4 */
        "stp x24, x23, [sp, #16]\n"
        "stp x22, x21, [sp, #32]\n"
        "adrp x16, _chain_probe_cont_dev@PAGE\n"
        "ldr x16, [x16, _chain_probe_cont_dev@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void chain_probe_devq_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x2, x0\n"                               /* the device object */
        "mov x0, #4\n"
        "mov x1, x30\n"
        "bl _chain_probe_step_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #48\n"                          /* the replaced prologue: 0x2dc84ec..0x2dc84f4 */
        "str x19, [sp, #16]\n"
        "stp x29, x30, [sp, #32]\n"
        "adrp x16, _chain_probe_cont_devq@PAGE\n"
        "ldr x16, [x16, _chain_probe_cont_devq@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void chain_probe_devqr_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x2, x0\n"                               /* w0 = the function's return value */
        "mov x0, #5\n"
        "mov x1, x30\n"
        "bl _chain_probe_step_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "ldp x29, x30, [sp, #32]\n"                  /* the replaced epilogue: 0x2dc8524..0x2dc852c */
        "ldr x19, [sp, #16]\n"
        "add sp, sp, #48\n"
        "adrp x16, _chain_probe_cont_devqr@PAGE\n"   /* -> 0x2dc8530: ret (x30 restored from the frame above) */
        "ldr x16, [x16, _chain_probe_cont_devqr@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void chain_probe_start_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x2, x0\n"                               /* the device object */
        "mov x3, x1\n"                               /* w1 = the start flag */
        "mov x0, #6\n"
        "mov x1, x30\n"
        "bl _chain_probe_step_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #64\n"                          /* the replaced prologue: 0x2dc8b50..0x2dc8b58 */
        "stp x22, x21, [sp, #16]\n"
        "stp x20, x19, [sp, #32]\n"
        "adrp x16, _chain_probe_cont_start@PAGE\n"
        "ldr x16, [x16, _chain_probe_cont_start@PAGEOFF]\n"
        "br x16\n");
}
void chain_probe_sndinit_note(uint64_t caller) {
    static int n;
    uint8_t *b = chain_probe_base;
    if (n++ == 0 && b) {
        uint8_t cfg = b[0x432d1c8]; uint64_t gate, arg, mgr;
        memcpy(&gate, b + 0x430a098, 8); memcpy(&arg, b + 0x430a1e0, 8); memcpy(&mgr, b + 0x430b298, 8);
        fprintf(stderr, "[aurchain] sound init wrapper main+0x2e33298 (caller main+0x%llx):"
                " cfg[0x432d1c8]=%#x gate[0x430a098]=%#llx arg[0x430a1e0]=%#llx mgr[0x430b298]=%#llx\n",
                (unsigned long long)(caller - (uint64_t)b),
                cfg, (unsigned long long)gate, (unsigned long long)arg, (unsigned long long)mgr);
        fflush(stderr);
    }
}
void chain_probe_step_note(int tag, uint64_t caller, uint64_t a1) {
    static int seen[8];
    if (tag < 0 || tag >= 8 || seen[tag]++ >= 4) return;
    uint8_t *b = chain_probe_base;
    unsigned long long co = b ? caller - (uint64_t)b : 0;
    const char *what =
        tag == 0 ? "sound init wrapper entered"
        : tag == 1 ? "sound manager create main+0x2e332ec"
        : tag == 2 ? "chain LAST step main+0x2dc903c (creates the AK Suspended thread)"
        : tag == 3 ? "AUDIO DEVICE SETUP main+0x2e33c9c"
        : tag == 4 ? "device query main+0x2dc84ec entry"
        : tag == 5 ? "device query main+0x2dc84ec RETURNS"
        : tag == 6 ? "device start main+0x2dc8b50 entry"
        : "chain runner reached its TAIL (step 8 + tail call to the last step)";
    if (tag == 5)
        fprintf(stderr, "[aurchain] %s w0=%llu (#%d)\n", what, (unsigned long long)a1, seen[tag]);
    else
        fprintf(stderr, "[aurchain] %s (#%d) caller main+0x%llx arg=%#llx\n", what, seen[tag], co, (unsigned long long)a1);
    fflush(stderr);
}
static int chain_install_one(uint8_t *at, const uint32_t expect[3], void **contp, uint8_t *cont,
                             const void *stub, const char *what) {
    uint32_t got[3]; memcpy(got, at, sizeof got);
    if (got[0] != expect[0] || got[1] != expect[1] || got[2] != expect[2]) {
        fprintf(stderr, "[aurchain] %s prologue differs (%#x %#x %#x): probe skipped\n", what, got[0], got[1], got[2]); return 0; }
    *contp = cont;
    uint32_t patch[3] = {0, 0, 0};
    emit_adrp_br(patch, (uintptr_t)at, stub);
    memcpy(at, patch, sizeof patch);
    sys_icache_invalidate(at, sizeof patch);
    fprintf(stderr, "[aurchain] %s probe installed at %p\n", what, (void *)at);
    return 1;
}
void runtime_audio_chain_probe_install(void) {
    if (!getenv("PLA_AUDIO_CHAIN_PROBE")) return;
    static int done; if (done) return; done = 1;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) { fprintf(stderr, "[aurchain] no nnMain\n"); return; }
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    chain_probe_base = base;
    chain_probe_sndinit_g = base + 0x430a000;
    chain_probe_g_df8ca8 = base + 0x2df8ca8;
    chain_probe_g_endstep = base + 0x2dc903c;
    static const uint32_t e_sndinit[3] = {0xa9bf7bfdu, 0x910003fdu, 0xf000a6a8u};
    static const uint32_t e_mgr[3]     = {0xf81e0ff3u, 0xa9017bfdu, 0x910043fdu};
    static const uint32_t e_tail[3]    = {0x97ff951au, 0xa8c17bfdu, 0x17fed5fdu};
    static const uint32_t e_end[3]     = {0xd100c3ffu, 0xa9014ff4u, 0xa9027bfdu};
    static const uint32_t e_dev[3]     = {0xa9bb67fau, 0xa9015ff8u, 0xa90257f6u};
    static const uint32_t e_devq[3]    = {0xd100c3ffu, 0xf9000bf3u, 0xa9027bfdu};
    static const uint32_t e_devqr[3]   = {0xa9427bfdu, 0xf9400bf3u, 0x9100c3ffu};
    static const uint32_t e_start[3]   = {0xd10103ffu, 0xa90157f6u, 0xa9024ff4u};
    chain_install_one(base + 0x2e33298, e_sndinit, &chain_probe_cont_sndinit, base + 0x2e332a4, chain_probe_sndinit_stub, "sound init wrapper");
    chain_install_one(base + 0x2e332ec, e_mgr, &chain_probe_cont_mgr, base + 0x2e332f8, chain_probe_mgr_stub, "manager create");
    chain_install_one(base + 0x2e13840, e_tail, &chain_probe_cont_tail, base + 0x2e13848, chain_probe_tail_stub, "chain runner tail");
    chain_install_one(base + 0x2dc903c, e_end, &chain_probe_cont_end, base + 0x2dc9048, chain_probe_end_stub, "chain last step");
    chain_install_one(base + 0x2e33c9c, e_dev, &chain_probe_cont_dev, base + 0x2e33ca8, chain_probe_dev_stub, "device setup");
    chain_install_one(base + 0x2dc84ec, e_devq, &chain_probe_cont_devq, base + 0x2dc84fc, chain_probe_devq_stub, "device query");
    chain_install_one(base + 0x2dc8524, e_devqr, &chain_probe_cont_devqr, base + 0x2dc8530, chain_probe_devqr_stub, "device query ret");
    chain_install_one(base + 0x2dc8b50, e_start, &chain_probe_cont_start, base + 0x2dc8b60, chain_probe_start_stub, "device start");
}

/* PLA_AUDIO_STATE_POLL=1 (diagnostic, zero guest perturbation): sample the sound engine's state words from a
   host thread and print transitions — the suspend flag (0x430a5b8, 1 = suspended), the suspend request (0x41aaf0),
   the boot gate (0x430a600, != 0 takes the suspend branch in the chain's last step), the registered-device count
   (0x430a7d0), the device-list head (0x430a7e0), the manager object (0x430b298) and the device-setup one-shot
   (0x430bd40). Code probes on this handshake perturb its timing (the suspend's signal races the AK Suspended
   thread's wait and can deadlock the boot); this watcher only reads. */
static void au_log_line(const char *fmt, ...);
static void au_dump_go_table(void);
static void *audio_state_poll(void *arg) {
    uint8_t *b = arg;
    static const struct { uint64_t off; const char *name; int w; } f[] = {
        {0x430a5b8, "suspended", 1}, {0x41aaf0, "suspend_req", 1}, {0x430a600, "boot_gate", 1},
        {0x430a7d0, "dev_count", 4}, {0x430a7e0, "dev_list", 8}, {0x430b298, "manager", 8}, {0x430bd40, "setup_once", 8},
        {0x430a248, "queue_count", 4}, {0x430a240, "queue_ptr", 8},
        {0x430a258, "cb_count", 4}, {0x430a250, "cb_ptr", 8}, {0x430a128, "tick_arg", 8}, {0x430a7c8, "ak_exit", 1},
        /* the Wwise bank-path gate registry (main+0x277e5f0 walks this; empty = banks never built) */
        {0x427b160, "bankgate_obj", 8}, {0x4279d90, "node_head", 8}, {0x4279dd0, "node_sentinel", 8},
        {0x42eeed0, "node_count", 4}, {0x42ec000, "node_tab0", 8}, {0x42ec008, "node_tab1", 8},
        {0x4279cd0, "bank_flag_cd0", 1}, {0x4279cd8, "bank_ptr_cd8", 8}, {0x4279ce0, "bank_ptr_ce0", 8},
    };
    uint64_t seen[22] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};   /* force one print of the initial state */
    uint64_t tick_frames = 0, tick_stamp = 0;
    for (unsigned s = 0; ; s++) {
        for (unsigned i = 0; i < 22; i++) {
            uint64_t v = 0;
            memcpy(&v, b + f[i].off, f[i].w);
            if (v != seen[i]) {
                seen[i] = v;
                fprintf(stderr, "[austate] t=%us %s = %#llx\n", s, f[i].name, (unsigned long long)v); fflush(stderr);
            }
        }
        {   /* the tick's own counters: x0 comes from [0x430a128]; [obj+0x90] stamp, [obj+0x98] idle frames, [obj+0xa0] cleared */
            uint64_t obj; memcpy(&obj, b + 0x430a128, 8);
            if (obj && obj < 0x100000000ULL) obj = 0;
            if (obj) {
                uint64_t st; uint32_t fr;
                memcpy(&st, (uint8_t *)(uintptr_t)obj + 0x90, 8);
                memcpy(&fr, (uint8_t *)(uintptr_t)obj + 0x98, 4);
                if (st != tick_stamp || fr != (uint32_t)tick_frames) {
                    tick_stamp = st; tick_frames = fr;
                    fprintf(stderr, "[austate] t=%us tick obj %#llx stamp %#llx frames %u\n",
                            s, (unsigned long long)obj, (unsigned long long)st, fr); fflush(stderr);
                }
            }
        }
        {   /* the bank-path gate object: [0x4279cd0] -> byte 0 bit0 must be set for the bank build to run */
            uint64_t p; memcpy(&p, b + 0x4279cd0, 8);
            static int last = -1;
            if (p && p > 0x100000000ULL) {
                int v = *(const volatile unsigned char *)(uintptr_t)p;
                if (v != last) { last = v; fprintf(stderr, "[austate] t=%us bankgate_byte0 = %#x\n", s, v); fflush(stderr); }
            }
        }
        {   /* the registered device's state word [dev+0x160]: 2 = initial, 1 = running */
            uint64_t dev; memcpy(&dev, b + 0x430a7e0, 8);
            static uint32_t last = 0xFFFFFFFFu;
            if (dev && dev > 0x100000000ULL) {
                uint32_t st; memcpy(&st, (uint8_t *)(uintptr_t)dev + 0x160, 4);
                uint64_t job; memcpy(&job, (uint8_t *)(uintptr_t)dev + 0x138, 8);
                if (st != last) { last = (int)st; fprintf(stderr, "[austate] t=%us dev_state = %u (job=%#llx)\n", s, st, (unsigned long long)job); fflush(stderr); }
            }
        }
        if ((s == 30 || s == 90) && getenv("PLA_DUMP_ENG")) {
            static const unsigned goff[] = {0x120,0x128,0x130,0x140,0x148,0x150,0x160,0x170,0x1a0,0x1c8,0x240,0x248,0x250,0x258,0x2d0,0x300,0x308,0x310,0x318,0x5b8,0x7d0,0x7e0,0xe00,0xe08,0xe30,0xe38,0xe40,0xe48,0xe58,0xe60,0xe68};
            au_log_line("[engdump] base=%p\n", (void *)b);
            for (unsigned i = 0; i < sizeof(goff)/sizeof(goff[0]); i++) {
                uint64_t v = 0; memcpy(&v, b + 0x430a000 + goff[i], 8);
                au_log_line("[engdump] +0x%03x = %#llx\n", goff[i], (unsigned long long)v);
            }
            {   uint8_t v = 0; memcpy(&v, b + 0x430a1cd, 1); au_log_line("[engdump] 430a1cd=%u  (nonzero => RenderAudio only signals, no render)\n", v); }
            {   uint64_t sink = 0; memcpy(&sink, b + 0x430a7e0, 8);
                if (sink) memcpy(&sink, (void *)(uintptr_t)(sink + 0x138), 8);
                if (sink) {
                    uint64_t vt = 0; memcpy(&vt, (void *)(uintptr_t)sink, 8);
                    au_log_line("[engdump] sink=%#llx vt=main+0x%llx\n", (unsigned long long)sink,
                                (unsigned long long)(vt - (uint64_t)(uintptr_t)b));
                    for (unsigned k = 0; k <= 0x80; k += 8) {
                        uint64_t sl = 0; memcpy(&sl, (void *)(uintptr_t)(vt + k), 8);
                        au_log_line("[engdump] sink.vt+0x%02x = main+0x%llx\n", k, (unsigned long long)(sl - (uint64_t)(uintptr_t)b));
                    }
                }
            }
            {   /* LEngine-ish region 0x430ae00..0x430ae80: voices, active list, device routes */
                char line[512]; int off = 0;
                off += snprintf(line+off, sizeof(line)-off, "[engdump] ae:");
                for (unsigned k = 0; k < 0x80 && off < (int)sizeof(line)-24; k += 8) {
                    uint64_t q = 0; memcpy(&q, b + 0x430ae00 + k, 8);
                    off += snprintf(line+off, sizeof(line)-off, " %s", q ? "P" : "0");
                }
                au_log_line("%s\n", line);
                for (unsigned k = 0; k < 0x80; k += 8) {
                    uint64_t q = 0; memcpy(&q, b + 0x430ae00 + k, 8);
                    if (q) au_log_line("[engdump] ae+0x%02x = %#llx\n", k, (unsigned long long)q);
                }
            }
            {   /* look for an LEngine-ish object reachable from the engine singleton */
                uint64_t eng = 0; memcpy(&eng, b + 0x430a120, 8);
                au_log_line("[lescan] engine glob=%#llx\n", (unsigned long long)eng);
                if (eng >= 0x100000000ULL && eng < 0x200000000ULL) {
                    for (unsigned k = 0; k < 0x400; k += 8) {
                        uint64_t q = 0; memcpy(&q, (void *)(uintptr_t)(eng + k), 8);
                        if (q < 0x100000000ULL || q >= 0x200000000ULL || (q & 7)) continue;
                        uint64_t vt = 0; memcpy(&vt, (void *)(uintptr_t)q, 8);
                        if (vt < (uint64_t)(uintptr_t)b + 0x4100000ULL || vt > (uint64_t)(uintptr_t)b + 0x4200000ULL) continue;
                        uint64_t vp = 0; memcpy(&vp, (void *)(uintptr_t)(q + 0x190), 8);
                        au_log_line("[lescan] eng+0x%03x -> %#llx vt=main+0x%llx +0x190=%#llx +0x170=%#llx\n",
                                    k, (unsigned long long)q,
                                    (unsigned long long)(vt - (uint64_t)(uintptr_t)b),
                                    (unsigned long long)vp, (unsigned long long)0ULL);
                    }
                }
            }
            {   uint32_t v = 0; memcpy(&v, b + 0x41ab25c, 4); au_log_line("[engdump] maxvoices[41ab25c]=%u\n", v); }
            {   /* the singleton the play-request interface methods forward to (TLS-cached fallback slot) */
                uint64_t drv = 0; memcpy(&drv, b + 0x42f1798, 8);
                uint64_t dvt = 0; if (drv) memcpy(&dvt, (void *)(uintptr_t)drv, 8);
                uint64_t c0 = 0; if (dvt) memcpy(&c0, (void *)(uintptr_t)(dvt + 0xc0), 8);
                uint64_t s0 = 0; if (dvt) memcpy(&s0, (void *)(uintptr_t)dvt, 8);
                au_log_line("[engdump] reqdrv=[42f1798]=%#llx vt=main+0x%llx vt+0x00=main+0x%llx vt+0xc0=main+0x%llx\n",
                            (unsigned long long)drv, (unsigned long long)(dvt - (uint64_t)(uintptr_t)b),
                            (unsigned long long)(s0 - (uint64_t)(uintptr_t)b), (unsigned long long)(c0 - (uint64_t)(uintptr_t)b));
            }
            {   uint32_t v = 0; memcpy(&v, b + 0x41aaf0, 4); au_log_line("[engdump] 41aaf0=%u\n", v); }
            /* The event manager object at [base+0x430a170] is a hash map of pending actions: bucket array at +0,
               bucket count at +8, live count at +0x10 (see 0x2ddcc24, which inserts with a 0.9 load factor).
               If the live count only ever grows, the actions are queued but never executed. */
            {   uint64_t jm = 0; memcpy(&jm, b + 0x430a170, 8);
                if (AU_PTR_OK(jm)) {
                    uint64_t arr = 0, cnt = 0; uint32_t nbuck = 0, live = 0, lf = 0;
                    memcpy(&arr, (void *)(uintptr_t)jm, 8);
                    memcpy(&nbuck, (void *)(uintptr_t)(jm + 8), 4);
                    memcpy(&live, (void *)(uintptr_t)(jm + 0x10), 4);
                    memcpy(&lf, (void *)(uintptr_t)(jm + 0x18), 4);
                    /* count reachable entries so "live" can be checked against the chains */
                    unsigned walk = 0;
                    if (AU_PTR_OK(arr) && nbuck && nbuck < 65536) {
                        for (unsigned i = 0; i < nbuck && walk < 4096; i++) {
                            uint64_t h = 0; memcpy(&h, (void *)(uintptr_t)(arr + (uint64_t)i * 8), 8);
                            for (int d = 0; d < 64 && AU_PTR_OK(h); d++) {
                                uint64_t nx = 0; memcpy(&nx, (void *)(uintptr_t)h, 8); h = nx; walk++; } } }
                    au_log_line("[engdump] actionmap=%#llx buckets=%u live=%u lf=%u entries=%u\n",
                                (unsigned long long)jm, nbuck, live, lf, walk);
                    (void)cnt; } }
            {   uint64_t head = 0; memcpy(&head, b + 0x430a7e0, 8);
                au_log_line("[engdump] devhead=%#llx\n", (unsigned long long)head);
                if (head) { for (unsigned k = 0; k < 2; k++) {
                    uint64_t d = head; if (k) memcpy(&d, (void *)(uintptr_t)(head + 0x10), 8);
                    if (!d) break;
                    uint64_t f[6] = {0}; uint32_t g[2] = {0};
                    memcpy(&f[0], (void *)(uintptr_t)(d + 0x8), 8);
                    memcpy(&f[1], (void *)(uintptr_t)(d + 0x28), 8);
                    memcpy(&f[2], (void *)(uintptr_t)(d + 0x138), 8);
                    memcpy(&f[3], (void *)(uintptr_t)(d + 0x148), 8);
                    memcpy(&f[4], (void *)(uintptr_t)(d + 0x150), 8);
                    memcpy(&g[0], (void *)(uintptr_t)(d + 0x14), 4);
                    memcpy(&g[1], (void *)(uintptr_t)(d + 0x18), 4);
                    au_log_line("[engdump] dev[%u]=%#llx +8=%#llx +0x14=%u +0x18=%u +0x28=%#llx +0x138=%#llx +0x148=%#llx +0x150=%#llx\n",
                        k, (unsigned long long)d, (unsigned long long)f[0], g[0], g[1],
                        (unsigned long long)f[1], (unsigned long long)f[2], (unsigned long long)f[3], (unsigned long long)f[4]);
                } }
            }
        }
        if ((s == 25 || s == 45 || s == 65) && getenv("PLA_DUMP_PLAY")) {
            /* main+0x2e0ed58(x0=[0x430a150], x1=play params, w2=1, w3=0) allocates a 0x50-byte
               "play request" and appends it to a vector inside the object at [0x430a150]: either
               x22 = that object (w3!=1) with base @+0x28, count @+0x30, cap @+0x34, or
               x22 = object+0x10 with base @+0x0, count @+0x8, cap @+0xc.  If the count keeps
               growing the requests are queued but never consumed -- that is the missing link. */
            uint64_t holder = 0; memcpy(&holder, b + 0x430a150, 8);
            if (!holder) { au_log_line("[playdump] t=%us holder=0\n", s); }
            else {
                uint32_t v[6] = {0};
                memcpy(&v[0], (void *)(uintptr_t)(holder + 0x08), 4);  /* alt base lo (we only log counts) */
                memcpy(&v[1], (void *)(uintptr_t)(holder + 0x10), 4);
                memcpy(&v[2], (void *)(uintptr_t)(holder + 0x18), 4);
                memcpy(&v[3], (void *)(uintptr_t)(holder + 0x1c), 4);
                memcpy(&v[4], (void *)(uintptr_t)(holder + 0x30), 4);
                memcpy(&v[5], (void *)(uintptr_t)(holder + 0x34), 4);
                uint64_t base0 = 0, base1 = 0;
                memcpy(&base0, (void *)(uintptr_t)(holder + 0x00), 8);
                memcpy(&base1, (void *)(uintptr_t)(holder + 0x28), 8);
                au_log_line("[playdump] t=%us holder=%#llx base0=%#llx cnt0=%u cap0=%u  base1=%#llx cnt1=%u cap1=%u other=+0x10:%u +0x18:%u +0x1c:%u\n",
                            s, (unsigned long long)holder, (unsigned long long)base0, v[0], 0u,
                            (unsigned long long)base1, v[4], v[5], v[1], v[2], v[3]);
                if (base1 && base1 > 0x100000000ULL && v[4] && v[4] < 64) {
                    for (uint32_t k = 0; k < v[4] && k < 3; k++) {
                        uint64_t e = 0; memcpy(&e, (void *)(uintptr_t)(base1 + 8ULL * k), 8);
                        au_log_line("[playdump]   req[%u]=%#llx\n", k, (unsigned long long)e);
                    }
                }
            }
        }
        if (s == 10 && getenv("PLA_DUMP_VT")) {
            static int done2;
            if (!done2) {
                done2 = 1;
                unsigned long long off = strtoull(getenv("PLA_DUMP_VT"), NULL, 0);
                fprintf(stderr, "[vtdump] at main+%#llx\n", off); fflush(stderr);
                for (int i = 0; i < 24; i++) {
                    uint64_t v = 0; memcpy(&v, b + off + 8ULL * (unsigned)i, 8);
                    if (!v) continue;
                    if (v > 0x100000000ULL && v < 0x100000000ULL + 0x1000000ULL)
                        v = (uint64_t)(uintptr_t)(b + (v - 0x100000000ULL));
                    fprintf(stderr, "[vtdump] +%#03x = main+%#llx\n", 8 * i,
                            (unsigned long long)(v - (uint64_t)(uintptr_t)b)); fflush(stderr);
                }
            }
        }
        if (s == 30 || s == 60 || s == 90 || s == 120 || s == 150 || s == 180) {
            extern unsigned au_ring_pend(uint8_t *mb);
            uint8_t *mb2 = b;
            if (mb2) {
                uint64_t eng0 = *(const uint64_t *)(mb2 + 0x430a120);
                uint64_t mp = (eng0 && eng0 < 0x40000000000ULL) ? *(const uint64_t *)(eng0 + 0x170) : 0;
                au_log_line("[actmap2] t=%d map=%#llx buckets=%llu live=%llu lf=%llu\n", s,
                            (unsigned long long)mp,
                            (unsigned long long)(mp ? *(const uint64_t *)(mp + 8) : 0),
                            (unsigned long long)(mp ? *(const uint64_t *)(mp + 0x10) : 0),
                            (unsigned long long)(mp ? *(const uint64_t *)(mp + 0x18) : 0));
                uint64_t rq = *(const uint64_t *)(mb2 + 0x430a128);
                au_log_line("[ringq] t=%d ring=%#llx f0=%#llx f8=%#llx f10=%#llx f18=%#llx f20=%#llx pend=%u\n",
                            s, (unsigned long long)rq,
                            (unsigned long long)(rq ? *(const uint64_t *)rq : 0),
                            (unsigned long long)(rq ? *(const uint64_t *)(rq + 8) : 0),
                            (unsigned long long)(rq ? *(const uint64_t *)(rq + 0x10) : 0),
                            (unsigned long long)(rq ? *(const uint64_t *)(rq + 0x18) : 0),
                            (unsigned long long)(rq ? *(const uint64_t *)(rq + 0x20) : 0),
                            au_ring_pend(mb2));
            }
        }
        if (s == 45 && getenv("PLA_DUMP_GO")) au_dump_go_table();
        if (s == 8 && getenv("PLA_DUMP_SLOT")) {
            /* the 24-byte {fn, slot, 0x403} record tables are applied to BSS slots at load time; verify a few
               known slots actually point at the function the record names (an off-by-one record would explain a
               lot: every audio API pointer would land on its neighbour). */
            static int done3;
            if (!done3) {
                done3 = 1;
                /* these are RELA entries {r_offset, r_info=0x403, r_addend} applied by loader.c relocate();
                   the expected value is therefore the record's r_addend, i.e. the *thunk* address. */
                const struct { uint32_t slot; uint32_t expect; const char *name; } t[] = {
                    {0x4176018, 0x2837788, "AK::API+0xe8"},
                    {0x4158a68, 0x1dd83ac, "sound-class+0x50"},
                    {0x4130108, 0x1848d74, "sound-class-slot"},
                    {0x4130100, 0x1844770, "sound-class-slot2"},
                    {0x4130110, 0x1848fb0, "sound-class-slot3"},
                };
                for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
                    uint64_t v = 0; memcpy(&v, b + t[i].slot, 8);
                    long long got = (long long)(v - (uint64_t)(uintptr_t)b);
                    au_log_line("[slot] %s slot=%#x expect=main+0x%x got=%s0x%llx%s\n",
                                t[i].name, t[i].slot, t[i].expect,
                                (got == (long long)t[i].expect) ? "main+" : "main+",
                                (unsigned long long)got,
                                (got == (long long)t[i].expect) ? "" : " MISMATCH");
                }
            }
        }
        if (s == 8 && getenv("PLA_DUMP_VTAB")) {
            /* dump the "0x403 record" slot blob: the loader fills entries here; the game's sound class family
               lives at blob+0x98 (scene update at +0x70) — a zero entry means the fixup was never applied. */
            static int done; 
            if (!done) {
                done = 1;
                fprintf(stderr, "[vtblob] dumping %#x bytes at main+0x4130000\n", 0x4000); fflush(stderr);
                {   /* sanity: is the very same 0x403 record mechanism applied anywhere at runtime? */
                    uint64_t v1 = 0, v2 = 0, v3 = 0;
                    memcpy(&v1, b + 0x4176018, 8);      /* AK::LoadBank slot (record table target) */
                    memcpy(&v2, b + 0x4158a68, 8);      /* scene-sound "scene init" slot */
                    memcpy(&v3, b + 0x4130108, 8);      /* scene update slot */
                    fprintf(stderr, "[vtblob] slots: loadbank_slot=%#llx(scene? %#llx) sceneinit_slot=%#llx sceneupd_slot=%#llx\n",
                            (unsigned long long)v1, (unsigned long long)(uintptr_t)(b + 0x2d77440),
                            (unsigned long long)v2, (unsigned long long)v3); fflush(stderr);
                }
                for (uint32_t o = 0; o < 0x4000; o += 8) {
                    uint64_t v; memcpy(&v, b + 0x4130000 + o, 8);
                    if (!v) continue;
                    if (v > 0x100000000ULL && v < 0x100000000ULL + 0x1000000ULL) v = b ? (uint64_t)(uintptr_t)(b + (v - 0x100000000ULL)) : v;
                    fprintf(stderr, "[vtblob] +%#05x = main+%#llx\n", o, (unsigned long long)(v - (uint64_t)(uintptr_t)b)); fflush(stderr);
                }
            }
        }
        if (s && s % 120 == 0) { fprintf(stderr, "[austate] t=%us alive\n", s); fflush(stderr); }
        usleep(100000);
    }
    return NULL;
}
void runtime_audio_state_poll_maybe_start(void) {
    { extern void runtime_bank_probe_install(void); runtime_bank_probe_install(); }
    if (!getenv("PLA_AUDIO_STATE_POLL") && !getenv("PLA_DUMP_ENG") && !getenv("PLA_DUMP_GO") &&
        !getenv("PLA_DUMP_VTAB") && !getenv("PLA_DUMP_PLAY") && !getenv("PLA_DUMP_VT") && !getenv("PLA_DUMP_SLOT")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) { fprintf(stderr, "[austate] no nnMain\n"); return; }
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    pthread_t th; pthread_create(&th, NULL, audio_state_poll, base); pthread_detach(th);
    fprintf(stderr, "[austate] watcher started\n");
}


/* PLA_AUDIO_BANK_PROBE=1 (diagnostic): the game builds its Wwise bank paths (Init.bnk etc.) at main+0x277e5d8+,
   gated on a registry walk (0x84cba4: follows the node list from [0x427b160]'s iterator to the sentinel
   [0x4279dd0]; the walk result decides whether the whole bank-path block runs at all — when it fails no bank
   is ever opened all boot). Patch the bl at main+0x277e5ec to a stub that dumps the registry and calls the
   original check, then replays the replaced tbz / mov exactly. */
static uint8_t *bank_probe_base;
uint64_t bank_probe_real, bank_probe_pass, bank_probe_fail;
void bank_probe_note(uint64_t arg, uint64_t result) {
    static int n;
    if (n++ >= 4 || !bank_probe_base) return;
    uint64_t obj = 0, head = 0, sentinel = 0; uint32_t count = 0;
    memcpy(&obj, bank_probe_base + 0x427b160, 8);
    memcpy(&head, bank_probe_base + 0x4279d90, 8);
    memcpy(&sentinel, bank_probe_base + 0x4279dd0, 8);
    memcpy(&count, bank_probe_base + 0x42eeed0, 4);
    fprintf(stderr, "[bankprobe] gate arg=%#llx -> result=%llu obj=%#llx head=%#llx sentinel=%#llx n=%u\n",
            (unsigned long long)arg, (unsigned long long)result, (unsigned long long)obj,
            (unsigned long long)head, (unsigned long long)sentinel, count);
    fflush(stderr);
}
uint64_t bank_gate_hook(uint64_t arg) {
    uint64_t (*real)(uint64_t) = (uint64_t (*)(uint64_t))bank_probe_real;
    uint64_t r = real(arg);
    bank_probe_note(arg, r);
    return r;
}
__attribute__((naked)) static void bank_gate_stub(void) {
    __asm__ volatile(
        "stp x29, x30, [sp, #-16]!\n"
        "bl _bank_gate_hook\n"                 /* x0 in/out; dumps the registry */
        "ldp x29, x30, [sp], #16\n"            /* guest's return address (0x277e5f0) — the tbz is replayed below */
        "tbz w0, #0, 1f\n"
        "mov w2, #0x210\n"                      /* replayed instruction */
        "adrp x16, _bank_probe_pass@PAGE\n"
        "ldr x16, [x16, _bank_probe_pass@PAGEOFF]\n"
        "br x16\n"
        "1:\n"
        "adrp x16, _bank_probe_fail@PAGE\n"
        "ldr x16, [x16, _bank_probe_fail@PAGEOFF]\n"
        "br x16\n");
}
void runtime_bank_probe_install(void) {
    if (!getenv("PLA_AUDIO_BANK_PROBE")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    bank_probe_base = base;
    bank_probe_real = (uint64_t)(uintptr_t)(base + 0x84cba4);
    bank_probe_pass = (uint64_t)(uintptr_t)(base + 0x277e5fc);
    bank_probe_fail = (uint64_t)(uintptr_t)(base + 0x277ed00);
    uint8_t *at = base + 0x277e5ec;   /* bl 0x84cba4 */
    uint32_t w; memcpy(&w, at, 4);
    if ((w & 0xFC000000) != 0x94000000) { fprintf(stderr, "[bankprobe] site differs (%#x)\n", w); return; }
    uint32_t patch[3] = {0, 0, 0};
    emit_adrp_br(patch, (uintptr_t)at, (const void *)bank_gate_stub);
    memcpy(at, patch, 12);
    sys_icache_invalidate(at, 12);
    fprintf(stderr, "[bankprobe] installed at %p\n", (void *)at);
}

/* PLA_AUDIO_BOOT_RESUME=1 (diagnostic): the sound chain's last step (main+0x2dc903c) suspends the engine at boot
   whenever the boot gate [x19+0x40 = 0x430a600] is set (it is, = 3): it calls the suspend/resume toggle
   (main+0x2dc9278) with w0 = 1 from main+0x2dc90c0, and the game never passes 0 anywhere (all three toggle call
   sites are hardwired to 1; a resume only comes through Wwise's own event processing, which needs a loaded bank —
   and no bank is ever requested while suspended). The engine then sits suspended forever: the tick stays in its
   idle branch, the sink (main+0x2e35b14) is never constructed, no AudioOut is ever opened. Boot the engine
   resumed instead: patch the `mov w0, #1` at main+0x2dc90b8 to 0. Memory polling only around this — code probes
   on this handshake perturb its timing and hang the boot. */
void runtime_audio_boot_resume_install(void) {
    /* On by default: without it the sound system stays silent for the whole session. PLA_AUDIO_SUSPEND=1
       restores the game's own boot-suspend behaviour. */
    if (getenv("PLA_AUDIO_SUSPEND")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) { fprintf(stderr, "[audboot] no nnMain\n"); return; }
    uint8_t *site = (uint8_t *)nnmain - 0x2779f70ULL + 0x2dc90b8ULL;
    uint32_t w; memcpy(&w, site, 4);
    if (w != 0x52800020u) { fprintf(stderr, "[audboot] boot-suspend site differs (%#x): not patched\n", w); return; }
    uint32_t m = 0x52800000u; memcpy(site, &m, 4); sys_icache_invalidate(site, 4);
    fprintf(stderr, "[audboot] boot suspend -> resume (main+0x2dc90b8: mov w0, #0)\n");
    /* PLA_AUDIO_NO_SUSPEND=1 (diagnostic): the no-device branch of the toggle (the one the boot call takes, before
       any device is registered) hardwires suspended = 1 (`cset w8, (w19==1); strb w8, [0x430a5b8]` at main+0x2dc92ac)
       regardless of the requested state — the engine is parked until a Wwise event resumes it, and none ever comes.
       Store wzr instead so the engine boots unsuspended; with suspend_req = 0 (above) the tick's idle path also
       never re-suspends (it skips the suspend when the request bit is clear). */
    if (!getenv("PLA_AUDIO_SUSPEND")) {
        uint8_t *site2 = (uint8_t *)nnmain - 0x2779f70ULL + 0x2dc92b0ULL;
        uint32_t w2; memcpy(&w2, site2, 4);
        if ((w2 & ~0x1Fu) == 0x3916E120u) { uint32_t w3 = w2 | 0x1Fu; memcpy(site2, &w3, 4); sys_icache_invalidate(site2, 4);
            fprintf(stderr, "[audboot] no-suspend branch now stores 0 (main+0x2dc92ac strb wzr)\n"); }
        else fprintf(stderr, "[audboot] no-suspend site differs (%#x): not patched\n", w2);
    }
}

void *runtime_audio_sdk_override(const char *name) {
    static const struct { const char *name; void *fn; } e[] = {
        {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", au_open_default},
        {"_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPKcRKNS0_17AudioOutParameterEPNS_2os11SystemEventE", au_open_named},
        {"_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", au_append},
        {"_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", au_set_buffer_info},
        {"_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", au_buffer_pointer},
        {"_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", au_released},
        {"_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", au_start},
        {"_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", au_stop},
        {"_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", au_close},
        {"_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", au_state},
        {"_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", au_channels},
    };
    for (size_t i = 0; i < sizeof e / sizeof e[0]; i++) if (!strcmp(name, e[i].name)) return e[i].fn;
    return NULL;
}

/* PLA_AUDIO_API_PROBE=1 (diagnostic): trampolines on the game's Wwise entry wrappers and the SoundController gate
   updater, all mapped statically (session of the LoadBank hunt; offsets pinned to this build like every probe here):
     main+0x2813338 setstate  — every AK::SoundEngine::SetState the game pushes (121 call sites funnel here);
                                x1 = interned name {u64 hash, char* name, u32 len}
     main+0x9b9db4  postevent — every PostEvent (7 call sites); x0 = interned name, w1 = flag; a 0 playingID
                                (event unknown = banks missing) makes the wrapper silently skip its bookkeeping
     main+0x2813898 setrtpc   — SetRTPCValue (14 sites); x1 = interned name
     main+0x2812fb8 loadbank  — the game's only LoadBank wrapper (engine vtable+0x100/0x108); x1 = name, w2 = async
     main+0x8417a8  gate      — the SoundController state updater: reads the app state at
                                *(*(x0+8)+0x98) -> +0x58 -> +0x10d4 and sets SoundController+0x54 (1 for app states
                                1/4 = banks may load, 3 for 3 = loading, 2 otherwise); logging the state shows the
                                gate's value over the boot without patching the store
   Each stub saves x0-x3 and the guest caller's x30, notes, restores, replays the site's three prologue words and
   continues at fn+12 (none of the replaced words is PC-relative). */
void api_note_setstate(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller);
void api_note_postevent(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller);
void api_note_setrtpc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t value, uint64_t caller);
void api_note_loadbank(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller);
void api_note_gate(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller);
static void *api_cont_setstate, *api_cont_postevent, *api_cont_setrtpc, *api_cont_loadbank, *api_cont_gate;
static uint8_t *api_probe_base;
static void api_print_name(const char *tag, uint64_t entry, uint64_t flag, uint64_t caller) {
    if (entry < 0x1000) { fprintf(stderr, "[apiprobe] %s entry=null caller main+0x%llx\n", tag, (unsigned long long)(caller - (uint64_t)api_probe_base)); return; }
    uint64_t np = *(uint64_t *)(entry + 8);
    uint32_t len = np ? *(uint32_t *)(entry + 0x10) : 0;
    if (len > 64) len = 64;
    fprintf(stderr, "[apiprobe] %s \"%.*s\" a2=%#llx caller main+0x%llx\n", tag, (int)len, np ? (const char *)np : "",
            (unsigned long long)flag, (unsigned long long)(caller - (uint64_t)api_probe_base));
}
void api_note_setstate(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller) { (void)a0;
    static int n;
    if (n++ < 64) api_print_name("setstate", a1, a2, caller);
}
/* One-shot dump of the game's AK API dispatch table: gameAudio+0x158 -> +0x60 -> object whose vtable holds the
   engine entry points at fixed offsets (+0x88 SetRTPC, +0xc0 PostEvent, +0xe0/+0xe8 UnloadBank, +0x100/+0x108 LoadBank).
   Printing every slot reveals which entry is RenderAudio (the one nothing calls in this port). */
static void api_dump_table(uint64_t gameaudio) {
    static int done;
    if (done) return;
    done = 1;
    if (gameaudio < 0x1000) { fprintf(stderr, "[apitab] gameaudio=null\n"); return; }
    uint64_t eng = *(uint64_t *)(gameaudio + 0x158);
    uint64_t api = eng > 0x1000 ? *(uint64_t *)(eng + 0x60) : 0;
    uint64_t vt = api > 0x1000 ? *(uint64_t *)(api) : 0;
    fprintf(stderr, "[apitab] gameaudio=%#llx engine=%#llx api=%#llx vt=%#llx\n",
            (unsigned long long)gameaudio, (unsigned long long)eng, (unsigned long long)api, (unsigned long long)vt);
    if (!vt) return;
    for (int off = 0; off <= 0x120; off += 8) {
        uint64_t fn = *(uint64_t *)(vt + off);
        fprintf(stderr, "[apitab] +0x%03x = main+0x%llx\n", off,
                (unsigned long long)(fn ? fn - (uint64_t)api_probe_base : 0));
    }
    fflush(stderr);
}

/* ---- AK API dispatch-table census (PLA_AUDIO_APITAB_PROBE=1) --------------------------------
   The game reaches the Wwise engine through gameAudio+0x158 -> +0x60 -> object whose vtable holds
   the engine entry points at fixed offsets (+0x88 SetRTPC, +0xc0 PostEvent, +0xe0 LoadBank,
   +0x100/+0x108 async LoadBank). Every occupied slot is replaced by a generated trampoline that
   counts and forwards, so one ordinary run tells us which entry point the game calls, how often
   and from where -- in particular whether anything calls RenderAudio, the stage that is missing
   here (akmix is never entered, the float mix buffer stays zero, no .wem/.pck is ever opened). */
static unsigned long g_apitab_count[48];
static int g_apitab_on;
void au_apitab_note(uint64_t caller, unsigned idx);
void au_apitab_note(uint64_t caller, unsigned idx) {
    if (idx >= 48) return;
    unsigned long n = ++g_apitab_count[idx];
    if (n <= 2 || (n & 0x1ff) == 0) {
        fprintf(stderr, "[apitab] slot +0x%03x n=%lu caller main+0x%llx\n", idx * 8, n,
                (unsigned long long)(caller - (uint64_t)api_probe_base));
        fflush(stderr);
    }
}
static void apitab_install(uint64_t gameaudio) {
    if (!g_apitab_on || gameaudio < 0x1000) return;
    uint64_t eng = *(uint64_t *)(gameaudio + 0x158);
    uint64_t api = eng > 0x1000 ? *(uint64_t *)(eng + 0x60) : 0;
    uint64_t *vt = api > 0x1000 ? *(uint64_t **)(api) : 0;
    if (!vt) { fprintf(stderr, "[apitab] no dispatch table\n"); return; }
    uint8_t *code = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED) { fprintf(stderr, "[apitab] mmap failed\n"); return; }
    int made = 0;
    for (int i = 0; i < 48; i++) {
        uint64_t orig = vt[i];
        if (!orig) continue;
        uint32_t *t = (uint32_t *)(code + i * 64);
        uintptr_t base = (uintptr_t)t;
        uintptr_t note = (uintptr_t)&au_apitab_note;
        t[0] = 0xa9bf07e0u;                      /* stp x0, x1, [sp, #-16]! */
        t[1] = 0xa9bf0fe0u;                      /* stp x2, x3, [sp, #-16]! */
        t[2] = 0xa9bf17e0u;                      /* stp x4, x5, [sp, #-16]! */
        t[3] = 0xf81f0ffeu;                      /* str x30, [sp, #-16]!    */
        t[4] = 0xaa1e03e0u;                      /* mov x0, x30             */
        t[5] = 0x52800001u | ((uint32_t)i << 5); /* mov w1, #idx            */
        int32_t d = (int32_t)((note - (base + 24)) >> 2);
        t[6] = 0x94000000u | (uint32_t)(d & 0x3ffffff);  /* bl note         */
        t[7] = 0xf84107feu;                      /* ldr x30, [sp], #16      */
        t[8] = 0xa8c117e0u;                      /* ldp x4, x5, [sp], #16   */
        t[9] = 0xa8c10fe0u;                      /* ldp x2, x3, [sp], #16   */
        t[10] = 0xa8c107e0u;                     /* ldp x0, x1, [sp], #16   */
        t[11] = 0x58000049u;                     /* ldr x9, [pc, #8]        */
        t[12] = 0xd61f0120u;                     /* br x9                   */
        *(uint64_t *)(t + 13) = orig;            /* literal                 */
        vt[i] = (uint64_t)t;
        made++;
    }
    sys_icache_invalidate(code, 0x1000);
    if (mprotect(code, 0x1000, PROT_READ | PROT_EXEC) != 0) fprintf(stderr, "[apitab] mprotect failed\n");
    fprintf(stderr, "[apitab] hooked %d engine slots at vt=%p\n", made, (void *)vt);
    fflush(stderr);
}
void api_note_postevent(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller) { (void)a2;
    api_dump_table(a0);
    static int n;
    if (n++ < 64) api_print_name("postevent", a0, a1, caller);
}
void api_note_setrtpc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t value, uint64_t caller) { (void)a0; (void)a3;
    static int n;
    if (n++ >= 16) return;
    if (a1 < 0x1000) { fprintf(stderr, "[apiprobe] setrtpc entry=null caller main+0x%llx\n", (unsigned long long)(caller - (uint64_t)api_probe_base)); return; }
    uint64_t np = *(uint64_t *)(a1 + 8);
    uint32_t len = np ? *(uint32_t *)(a1 + 0x10) : 0;
    if (len > 64) len = 64;
    union { uint32_t u; float f; } v; v.u = (uint32_t)value;
    fprintf(stderr, "[apiprobe] setrtpc \"%.*s\" value=%g (bits %#x) gameobj=%#llx caller main+0x%llx\n", (int)len,
            np ? (const char *)np : "", (double)v.f, v.u, (unsigned long long)a2,
            (unsigned long long)(caller - (uint64_t)api_probe_base));
    fflush(stderr);
}
void api_note_loadbank(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller) { (void)a0;
    static int n;
    if (n++ < 200) api_print_name("loadbank", a1, a2, caller);
}
void api_note_gate(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller) { (void)a1; (void)a2;
    static int n;
    if (n > 10 && (n - 11) % 900) { n++; return; }
    n++;
    uint64_t t1 = a0 ? *(uint64_t *)(a0 + 8) : 0;
    uint64_t t2 = t1 ? *(uint64_t *)(t1 + 0x98) : 0;
    uint64_t t3 = t2 ? *(uint64_t *)(t2 + 0x58) : 0;
    int state = t3 ? *(int *)(t3 + 0x10d4) : -1;
    fprintf(stderr, "[apiprobe] gate app_state=%d (predicts ctrl+0x54=%s) caller main+0x%llx\n", state,
            state == 1 || state == 4 ? "1" : state == 3 ? "3" : "2", (unsigned long long)(caller - (uint64_t)api_probe_base));
}
__attribute__((naked)) static void api_setstate_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"                      /* the guest caller's return address; bl below overwrites x30 */
        "mov x3, x30\n"
        "bl _api_note_setstate\n"
        "ldr x30, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #0x80\n"                         /* the prologue at main+0x2813338..0x2813340, replayed whole */
        "str x25, [sp, #0x30]\n"
        "stp x24, x23, [sp, #0x40]\n"
        "adrp x16, _api_cont_setstate@PAGE\n"
        "ldr x16, [x16, _api_cont_setstate@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void api_postevent_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x3, x30\n"
        "bl _api_note_postevent\n"
        "ldr x30, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #0xc0\n"                         /* the prologue at main+0x9b9db4..0x9b9dbc, replayed whole */
        "stp x22, x21, [sp, #0x90]\n"
        "stp x20, x19, [sp, #0xa0]\n"
        "adrp x16, _api_cont_postevent@PAGE\n"
        "ldr x16, [x16, _api_cont_postevent@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void api_setrtpc_stub(void) {
    __asm__ volatile(
        "str s0, [sp, #-16]!\n"                      /* the RTPC value rides in s0; the wrapper forwards it */
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "ldr w4, [sp, #48]\n"
        "mov x5, x30\n"
        "bl _api_note_setrtpc\n"
        "ldr x30, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "ldr s0, [sp], #16\n"
        "stp x29, x30, [sp, #-0x10]!\n"               /* the prologue at main+0x2813898..0x28138a0, replayed whole */
        "mov x29, sp\n"
        "ldr x8, [x0, #0x158]\n"
        "adrp x16, _api_cont_setrtpc@PAGE\n"
        "ldr x16, [x16, _api_cont_setrtpc@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void api_loadbank_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x3, x30\n"
        "bl _api_note_loadbank\n"
        "ldr x30, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "str x21, [sp, #-0x30]!\n"                    /* the prologue at main+0x2812fb8..0x2812fc0, replayed whole */
        "stp x20, x19, [sp, #0x10]\n"
        "stp x29, x30, [sp, #0x20]\n"
        "adrp x16, _api_cont_loadbank@PAGE\n"
        "ldr x16, [x16, _api_cont_loadbank@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void api_gate_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x3, x30\n"
        "bl _api_note_gate\n"
        "ldr x30, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "str x21, [sp, #-0x30]!\n"                    /* the prologue at main+0x8417a8..0x8417b0, replayed whole */
        "stp x20, x19, [sp, #0x10]\n"
        "stp x29, x30, [sp, #0x20]\n"
        "adrp x16, _api_cont_gate@PAGE\n"
        "ldr x16, [x16, _api_cont_gate@PAGEOFF]\n"
        "br x16\n");
}
void api_note_raw(int site, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller);
static void api_try_name(char *out, size_t cap, uint64_t entry) {
    out[0] = 0;
    if (entry < 0x1000) return;
    uint64_t np = *(uint64_t *)(entry + 8);
    if (np < 0x1000) return;
    uint32_t len = *(uint32_t *)(entry + 0x10);
    if (len > cap - 1) len = (uint32_t)cap - 1;
    memcpy(out, (const void *)np, len);
    out[len] = 0;
}
void api_note_raw(int site, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller) {
    static int n;
    if (n++ > 120 && (n - 121) % 400) return;
    char nm[64]; api_try_name(nm, sizeof nm, a1);
    fprintf(stderr, "[apiprobe] impl#%d a0=%#llx a1=%#llx a2=%#llx name=\"%s\" caller main+0x%llx\n", site,
            (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, nm,
            (unsigned long long)(caller - (uint64_t)api_probe_base));
}
__attribute__((naked)) static void api_impl1_stub(void) {          /* 0x2dcb224 engine LoadBank sync impl */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, #1\n""bl _api_note_raw\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #0x50\n""stp x24, x23, [sp, #0x10]\n""stp x22, x21, [sp, #0x20]\n"
        "adrp x16, _api_cont_impl1@PAGE\n""ldr x16, [x16, _api_cont_impl1@PAGEOFF]\n""br x16\n");
}
__attribute__((naked)) static void api_impl2_stub(void) {          /* 0x2dcb9b8 engine LoadBank async impl */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, #2\n""bl _api_note_raw\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #0x50\n""stp x22, x21, [sp, #0x20]\n""stp x20, x19, [sp, #0x30]\n"
        "adrp x16, _api_cont_impl2@PAGE\n""ldr x16, [x16, _api_cont_impl2@PAGEOFF]\n""br x16\n");
}
__attribute__((naked)) static void api_impl3_stub(void) {          /* 0x2dcb558 engine Prepare sync impl */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, #3\n""bl _api_note_raw\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "stp x20, x19, [sp, #-0x20]!\n""stp x29, x30, [sp, #0x10]\n""add x29, sp, #0x10\n"
        "adrp x16, _api_cont_impl3@PAGE\n""ldr x16, [x16, _api_cont_impl3@PAGEOFF]\n""br x16\n");
}
__attribute__((naked)) static void api_impl4_stub(void) {          /* 0x2dcb664 engine Prepare async impl */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, #4\n""bl _api_note_raw\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "str x21, [sp, #-0x30]!\n""stp x20, x19, [sp, #0x10]\n""stp x29, x30, [sp, #0x20]\n"
        "adrp x16, _api_cont_impl4@PAGE\n""ldr x16, [x16, _api_cont_impl4@PAGEOFF]\n""br x16\n");
}
__attribute__((naked)) static void api_impl5_stub(void) {          /* 0x281304c small wrapper A (caller 0xf91160) */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, #5\n""bl _api_note_raw\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "str x23, [sp, #-0x40]!\n""stp x22, x21, [sp, #0x10]\n""stp x20, x19, [sp, #0x20]\n"
        "adrp x16, _api_cont_impl5@PAGE\n""ldr x16, [x16, _api_cont_impl5@PAGEOFF]\n""br x16\n");
}
__attribute__((naked)) static void api_impl6_stub(void) {          /* 0x28130c4 small wrapper B (caller 0xf91160) */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, #6\n""bl _api_note_raw\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #0x40\n""stp x29, x30, [sp, #0x30]\n""add x29, sp, #0x30\n"
        "adrp x16, _api_cont_impl6@PAGE\n""ldr x16, [x16, _api_cont_impl6@PAGEOFF]\n""br x16\n");
}
static void *api_cont_impl1, *api_cont_impl2, *api_cont_impl3, *api_cont_impl4, *api_cont_impl5, *api_cont_impl6;
__attribute__((naked)) static void api_postevent_pos_stub(void) {   /* 0x281356c PostEvent at position */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, #12\n""bl _api_note_raw\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #0xb0\n""stp x28, x27, [sp, #0x50]\n""stp x26, x25, [sp, #0x60]\n"
        "adrp x16, _api_cont_pepos@PAGE\n""ldr x16, [x16, _api_cont_pepos@PAGEOFF]\n""br x16\n");
}
__attribute__((naked)) static void api_bankload_async_stub(void) {  /* 0x8dbe34 gfl bank load (async, callback) */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, #13\n""bl _api_note_raw\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #0xc0\n""stp x20, x19, [sp, #0xa0]\n""stp x29, x30, [sp, #0xb0]\n"
        "adrp x16, _api_cont_bla@PAGE\n""ldr x16, [x16, _api_cont_bla@PAGEOFF]\n""br x16\n");
}
__attribute__((naked)) static void api_bankload_sync_stub(void) {   /* 0x8df3bc gfl bank load (sync) */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, #14\n""bl _api_note_raw\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #0xc0\n""stp x20, x19, [sp, #0xa0]\n""stp x29, x30, [sp, #0xb0]\n"
        "adrp x16, _api_cont_bls@PAGE\n""ldr x16, [x16, _api_cont_bls@PAGEOFF]\n""br x16\n");
}
static void *api_cont_pepos, *api_cont_bla, *api_cont_bls;
void api_note_io(int which, uint64_t a0, uint64_t a1, uint64_t caller);
void api_note_io(int which, uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n[4];
    int w = which & 3;
    if (n[w]++ > 60) return;
    uint32_t type = 0;
    if (a1 && a1 < 0x7ffffff00000ULL) { uint32_t v; memcpy(&v, (const void *)a1, 4); type = v; }
    fprintf(stderr, "[apiprobe] io#%d req_type=%u a0=%#llx caller main+0x%llx\n", which, type,
            (unsigned long long)a0, (unsigned long long)(caller - (uint64_t)api_probe_base));
}
__attribute__((naked)) static void api_iodisp_stub(void) {   /* 0x2d8f2c8 IO request dispatcher (x1 = request) */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, #1\n""bl _api_note_io\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "str x28, [sp, #-0x20]!\n""stp x29, x30, [sp, #0x10]\n""add x29, sp, #0x10\n"
        "adrp x16, _api_cont_iodisp@PAGE\n""ldr x16, [x16, _api_cont_iodisp@PAGEOFF]\n""br x16\n");
}
__attribute__((naked)) static void api_bankopen_stub(void) {  /* 0x2d90504 bank open (BKHD version check lives here) */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, #2\n""bl _api_note_io\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #0x100\n""stp x28, x27, [sp, #0xa0]\n""stp x26, x25, [sp, #0xb0]\n"
        "adrp x16, _api_cont_bankopen@PAGE\n""ldr x16, [x16, _api_cont_bankopen@PAGEOFF]\n""br x16\n");
}
static void *api_cont_iodisp, *api_cont_bankopen;
void api_note_enq(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller);
void api_note_enq(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller) {
    static int n;
    (void)a0; (void)a2;
    if (n++ > 12) return;
    uint64_t w[8]; memset(w, 0, sizeof w);
    if (a1 > 0x1000 && a1 < 0x7ffffff00000ULL) memcpy(w, (const void *)a1, sizeof w);
    fprintf(stderr, "[apiprobe] enqueue caller main+0x%llx desc=%s%016llx %016llx %016llx %016llx\n",
            (unsigned long long)(caller - (uint64_t)api_probe_base), "0x",
            (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3]);
}
__attribute__((naked)) static void api_enq_stub(void) {   /* 0x2e0e3bc bank manager request enqueue (vtable+0x20) */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, x0\n""mov x0, #9\n""bl _api_note_enq\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "stp x22, x21, [sp, #-0x30]!\n""stp x20, x19, [sp, #0x10]\n""stp x29, x30, [sp, #0x20]\n"
        "adrp x16, _api_cont_enq@PAGE\n""ldr x16, [x16, _api_cont_enq@PAGEOFF]\n""br x16\n");
}
static void *api_cont_enq;
void api_note_bankname(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller);
void api_note_bankname(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t caller) {
    static int n;
    (void)a0; (void)a1;
    if (n++ > 40) return;
    const char *nm = (const char *)a2;
    char buf[96] = "";
    if (a2 > 0x1000 && a2 < 0x7ffffff00000ULL) {
        size_t i = 0;
        for (; i + 1 < sizeof buf; i++) { char c = nm[i]; if (!c) break; buf[i] = (c >= 32 && c < 127) ? c : '?'; }
        buf[i] = 0;
    }
    fprintf(stderr, "[apiprobe] LoadBank(\"%s\") caller main+0x%llx\n", buf,
            (unsigned long long)(caller - (uint64_t)api_probe_base));
}
static void *api_bank_target;   /* guest 0x2d77440, the engine's LoadBank(name, id*) */
__attribute__((naked)) static void api_bankname_stub(void) {   /* thunk 0x283777c: mov x0,x1; mov x1,x2; b LoadBank */
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n""stp x2, x3, [sp, #-16]!\n""str x30, [sp, #-16]!\n"
        "mov x3, x30\n""mov x2, x1\n""mov x1, x0\n""mov x0, #10\n""bl _api_note_bankname\n"
        "ldr x30, [sp], #16\n""ldp x2, x3, [sp], #16\n""ldp x0, x1, [sp], #16\n"
        "mov x0, x1\n""mov x1, x2\n"
        "adrp x16, _api_bank_target@PAGE\n""ldr x16, [x16, _api_bank_target@PAGEOFF]\n""br x16\n");
}
/* PLA_AUDIO_POSTCALL_PROBE=1 (diagnostic): patch the wrapper's engine call itself (main+0x281349c `blr x8`, followed
   by `mov w24, w0` and `cbz w0, <normal return>`), so the engine's real AKRESULT and the real vtable target are logged
   for every PostEvent. The three replaced words are re-implemented here: call, put the result into w24, then go to the
   failure body (main+0x28134a8) when it is non-zero and to the normal return (main+0x28134c8) when it is zero. The
   9th argument of the wrapped call (main+0x2813498 `str wzr, [sp]`) is re-created in a fresh slot below sp. */
void api_note_postcall_in(uint64_t target, uint64_t eventid, uint64_t flags, uint64_t caller);
void api_note_postcall_out(uint64_t target, uint64_t result);
static void *api_cont_postcall_ok, *api_cont_postcall_fail;
void api_note_postcall_in(uint64_t target, uint64_t eventid, uint64_t flags, uint64_t caller) {
    static int n;
    if (n++ >= 12) return;
    fprintf(stderr, "[apiprobe] postcall target main+0x%llx id=%#llx flags=%#llx caller main+0x%llx\n",
            (unsigned long long)(target - (uint64_t)api_probe_base), (unsigned long long)eventid,
            (unsigned long long)flags, (unsigned long long)(caller - (uint64_t)api_probe_base));
    fflush(stderr);
}
void api_note_postcall_out(uint64_t target, uint64_t result) {
    static int n;
    if (n++ >= 12) return;
    fprintf(stderr, "[apiprobe] postcall main+0x%llx -> AKRESULT %lld\n",
            (unsigned long long)(target - (uint64_t)api_probe_base), (long long)(int)result);
    fflush(stderr);
}
__attribute__((naked)) static void api_postcall_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "stp x4, x5, [sp, #-16]!\n"
        "stp x6, x7, [sp, #-16]!\n"
        "stp x8, x30, [sp, #-16]!\n"                 /* [sp] = vtable target, [sp+8] = the wrapper's return address */
        "mov x0, x8\n"
        "mov x1, x2\n"
        "mov x2, x3\n"
        "ldr x3, [sp, #8]\n"
        "bl _api_note_postcall_in\n"
        "ldp x8, x30, [sp], #16\n"
        "ldp x6, x7, [sp], #16\n"
        "ldp x4, x5, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #32\n"
        "str wzr, [sp]\n"                            /* the 9th argument */
        "str x8, [sp, #8]\n"
        "blr x8\n"
        "str w0, [sp, #16]\n"
        "ldr x0, [sp, #8]\n"
        "ldr w1, [sp, #16]\n"
        "bl _api_note_postcall_out\n"
        "ldr w24, [sp, #16]\n"                       /* the replaced `mov w24, w0` */
        "add sp, sp, #32\n"
        "cbnz w24, 2f\n"                             /* the replaced `cbz w0, <normal return>` */
        "adrp x16, _api_cont_postcall_ok@PAGE\n"
        "ldr x16, [x16, _api_cont_postcall_ok@PAGEOFF]\n"
        "br x16\n"
        "2:\n"
        "adrp x16, _api_cont_postcall_fail@PAGE\n"
        "ldr x16, [x16, _api_cont_postcall_fail@PAGEOFF]\n"
        "br x16\n");
}
void runtime_audio_postcall_probe_install(void);
void runtime_audio_postcall_probe_install(void) {
    if (!getenv("PLA_AUDIO_POSTCALL_PROBE")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) { fprintf(stderr, "[apiprobe] no nnMain\n"); return; }
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    if (!api_probe_base) api_probe_base = base;
    uint8_t *fn = base + 0x281349cULL;
    uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
    if (ex[0] != 0xd63f0100u || ex[1] != 0x2a0003f8u || ex[2] != 0x34000120u) {
        fprintf(stderr, "[apiprobe] postcall site differs (%#x %#x %#x), skipped\n", ex[0], ex[1], ex[2]); return; }
    api_cont_postcall_ok = base + 0x28134c8ULL;      /* mov w0, w24; ldp x29, x30, ... */
    api_cont_postcall_fail = fn + 12;                /* ldr x0, [x23, #0x178]; ... bl 0x281629c */
    uint32_t pt[3] = {0, 0, 0};
    emit_adrp_br(pt, (uintptr_t)fn, (const void *)api_postcall_stub);
    memcpy(fn, pt, sizeof pt);
    sys_icache_invalidate(fn, sizeof pt);
    fprintf(stderr, "[apiprobe] postcall probe installed at %p (ok %p fail %p)\n", (void *)fn, api_cont_postcall_ok, api_cont_postcall_fail);
    fflush(stderr);
}

/* The wrapper's failure branch (main+0x28134a8) calls main+0x281629c with w2 = the AKRESULT that
   AK::SoundEngine::PostEvent returned, x3 = the game object id, x1 = the interned-name record
   {u64 hash, char* name, u32 len}. So this entry is the engine's "I rejected your event" report: logging it
   separates "no event ever reaches the engine / every event is refused" from "events are accepted but no voice
   makes it into the mix". */
void api_note_postfail(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t caller);
static void *api_cont_postfail;
void api_note_postfail(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t caller) {
    static int n;
    (void)a0; (void)a4; (void)a5; (void)a6; (void)a7;
    if (n++ >= 200) return;
    char name[80]; name[0] = 0;
    if (a1 > 0x1000 && a1 < 0x1000000000ULL) {
        const char *np = *(const char **)(a1 + 8);
        uint32_t len = *(uint32_t *)(a1 + 0x10);
        if (np && len && len < sizeof name - 1) { memcpy(name, np, len); name[len] = 0; }
    }
    fprintf(stderr, "[apiprobe] postfail #%d a2=%#llx a3=%#llx a4=%#llx a5=%#llx \"%s\" caller main+0x%llx\n",
            n, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)a4,
            (unsigned long long)a5, name, (unsigned long long)(caller - (uint64_t)api_probe_base));
    fflush(stderr);
}
__attribute__((naked)) static void api_postfail_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "stp x4, x5, [sp, #-16]!\n"
        "stp x6, x7, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"                      /* the guest caller's return address; bl below overwrites x30 */
        "mov x9, x30\n"
        "sub sp, sp, #16\n"                           /* the 9th argument (caller) travels on the stack */
        "str x9, [sp]\n"
        "bl _api_note_postfail\n"
        "add sp, sp, #16\n"
        "ldr x30, [sp], #16\n"
        "ldp x6, x7, [sp], #16\n"
        "ldp x4, x5, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #0x130\n"                        /* the prologue at main+0x281629c..0x28162a4, replayed whole */
        "stp x28, x27, [sp, #0xd0]\n"
        "stp x26, x25, [sp, #0xe0]\n"
        "adrp x16, _api_cont_postfail@PAGE\n"
        "ldr x16, [x16, _api_cont_postfail@PAGEOFF]\n"
        "br x16\n");
}
/* PLA_AUDIO_FILL_PROBE=1 (diagnostic): the sink renders its buffer two ways. main+0x2e35ec8 converts the
   engine's float mix buffer (sink+0x28) into int16 and hands it to AppendAudioOutBuffer; main+0x2e35fd8 memsets
   the buffer to zero and appends that instead. Counting both says whether "PCM is all zero" means "the engine
   mixes silence" or "the game never asks the engine to mix at all". */
static int g_fillmix_n, g_fillsil_n;
void au_note_fill_mix(uint64_t self);
void au_note_fill_mix(uint64_t self) {
    if (!getenv("PLA_AUDIO_FILL_PROBE")) return;
    int n = ++g_fillmix_n;
    if (n > 4 && n % 500) return;
    uint64_t fb = 0; float f[8] = {0};
    if (self > 0x1000) {
        fb = *(uint64_t *)(uintptr_t)(self + 0x28);
        if (fb > 0x1000) memcpy(f, (const void *)(uintptr_t)fb, sizeof f);
    }
    fprintf(stderr, "[fill] mix #%d self=%p floatbuf=%#llx %g %g %g %g | %g %g %g %g (silence fills %d)\n",
            n, (void *)(uintptr_t)self, (unsigned long long)fb, (double)f[0], (double)f[1], (double)f[2], (double)f[3],
            (double)f[4], (double)f[5], (double)f[6], (double)f[7], g_fillsil_n);
    fflush(stderr);
}
void au_note_fill_sil(uint64_t self, uint64_t caller);
void au_note_fill_sil(uint64_t self, uint64_t caller) {
    if (!getenv("PLA_AUDIO_FILL_PROBE")) return;
    int n = ++g_fillsil_n;
    if (n > 4 && n % 2000) return;
    uint64_t fb = self > 0x1000 ? *(uint64_t *)(uintptr_t)(self + 0x28) : 0;
    float f[4] = {0};
    if (fb > 0x1000) memcpy(f, (const void *)(uintptr_t)fb, sizeof f);
    fprintf(stderr, "[fill] silence #%d self=%p mixfreq=%d floatbuf=%#llx %g %g %g %g caller main+0x%llx\n",
            n, (void *)(uintptr_t)self, g_fillmix_n, (unsigned long long)fb,
            (double)f[0], (double)f[1], (double)f[2], (double)f[3],
            (unsigned long long)(caller - (uint64_t)api_probe_base));
    fflush(stderr);
}
static void *fill_mix_cont, *fill_sil_cont;
__attribute__((naked)) static void fill_mix_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "bl _au_note_fill_mix\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "str x21, [sp, #-0x30]!\n"
        "stp x20, x19, [sp, #0x10]\n"
        "stp x29, x30, [sp, #0x20]\n"
        "add x29, sp, #0x20\n"
        "adrp x16, _fill_mix_cont@PAGE\n"
        "ldr x16, [x16, _fill_mix_cont@PAGEOFF]\n"
        "br x16\n");
}
__attribute__((naked)) static void fill_sil_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "ldr x1, [sp]\n"
        "bl _au_note_fill_sil\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "stp x20, x19, [sp, #-0x20]!\n"
        "stp x29, x30, [sp, #0x10]\n"
        "add x29, sp, #0x10\n"
        "adrp x16, _fill_sil_cont@PAGE\n"
        "ldr x16, [x16, _fill_sil_cont@PAGEOFF]\n"
        "br x16\n");
}
void runtime_audio_fill_probe_install(void);
void runtime_audio_fill_probe_install(void) {
    if (!getenv("PLA_AUDIO_FILL_PROBE")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    if (!api_probe_base) api_probe_base = base;
    struct { uint64_t off; uint32_t w[3]; void *stub; void **cont; uint64_t cont_off; const char *tag; } sites[] = {
        {0x2e35ec8, {0xf81d0ff5u, 0xa9014ff4u, 0xa9027bfdu}, (void *)fill_mix_stub, &fill_mix_cont, 0x2e35ed8ULL, "fillmix"},
        {0x2e35fd8, {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, (void *)fill_sil_stub, &fill_sil_cont, 0x2e35fe4ULL, "fillsil"},
    };
    for (unsigned i = 0; i < sizeof sites / sizeof sites[0]; i++) {
        uint8_t *fn = base + sites[i].off;
        uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
        if (ex[0] != sites[i].w[0] || ex[1] != sites[i].w[1] || ex[2] != sites[i].w[2]) {
            fprintf(stderr, "[fill] %s prologue differs (%#x %#x %#x), skipped\n", sites[i].tag, ex[0], ex[1], ex[2]);
            continue;
        }
        *sites[i].cont = base + sites[i].cont_off;
        uint32_t pt[3] = {0, 0, 0};
        emit_adrp_br(pt, (uintptr_t)fn, sites[i].stub);
        memcpy(fn, pt, sizeof pt);
        sys_icache_invalidate(fn, sizeof pt);
        fprintf(stderr, "[fill] %s probe installed at %p\n", sites[i].tag, (void *)fn);
    }
    fflush(stderr);
}

/* PLA_AUDIO_SITE_PROBE=1 (diagnostic): count entries of the game's own sound-startup chain
   (games/pla/CHANGELOG + /tmp/pla_re/report_gate.md). The chain is:
   mgr ctor 0x83fc18 -> step0 0x8405bc (readiness 0x8470dc) -> step1 0x8405f8 -> task 0x841384 ->
   SoundController ctor 0x8413a0 (writes component+0x54 through 0x8417a8) -> per-frame driver 0x841b58,
   with the chain start 0x84e4c0 (writes step 0x8405bc into mgr+0x100) and its consumer gate 0x853f1c.
   Zero entries on 0x84e4c0 = the chain never starts, which is why the sink only ever fills silence. */
static int g_site_n[16];
static const char *g_site_tag[16] = {"mgrctor", "chainstart", "step0", "step1", "SCctor", "framedrv", "ready", "gateload",
                                     "sinkok", "sinkdtor", "rendertick", "akmix",
                                     "sndmgrcreate", "chainlast", "devsetup", "chainstepA"};
void au_note_site(uint64_t idx, uint64_t self, uint64_t caller);
void au_note_site(uint64_t idx, uint64_t self, uint64_t caller) {
    if (!getenv("PLA_AUDIO_SITE_PROBE") || idx > 15) return;
    int n = ++g_site_n[idx];
    if (n > 3 && n % 300) return;
    if (idx == 1 && self > 0x1000) {
        /* 0x84e4c0 is a step-task driver: [self+0xf8] is its state, [self+0xb0] its manager, and the manager's
           +0x100 step function pointer (0x84e898 stores the sound chain's step 0x8405bc there) plus a +0x110 word. */
        uint32_t state = *(uint32_t *)(uintptr_t)(self + 0xf8);
        uint64_t mgr = *(uint64_t *)(uintptr_t)(self + 0xb0);
        uint64_t step = mgr > 0x1000 ? *(uint64_t *)(uintptr_t)(mgr + 0x100) : 0;
        uint32_t guard = mgr > 0x1000 ? *(uint32_t *)(uintptr_t)(mgr + 0x110) : 0;
        uint64_t vt = *(uint64_t *)(uintptr_t)self;
        fprintf(stderr, "[site] %s #%d self=%#llx rawvt=%#llx rawm70=%#llx rawm68=%#llx rawm78=%#llx name=%#llx\n",
                g_site_tag[idx], n, (unsigned long long)self, (unsigned long long)vt,
                (unsigned long long)(vt ? *(uint64_t *)(uintptr_t)(vt + 0x70) : 0),
                (unsigned long long)(vt ? *(uint64_t *)(uintptr_t)(vt + 0x68) : 0),
                (unsigned long long)(vt ? *(uint64_t *)(uintptr_t)(vt + 0x78) : 0),
                (unsigned long long)(self ? *(uint64_t *)(uintptr_t)(self + 0x10) : 0));
        fprintf(stderr, "[site] %s #%d self=%#llx vt=main+0x%llx m70=main+0x%llx state@0xf8=%u mgr=%#llx step@mgr0x100=%#llx guard=%#x caller main+0x%llx\n",
                g_site_tag[idx], n, (unsigned long long)self,
                (unsigned long long)au_modoff((void *)(uintptr_t)vt),
                (unsigned long long)au_modoff((void *)(uintptr_t)(vt ? *(uint64_t *)(uintptr_t)(vt + 0x70) : 0)),
                state, (unsigned long long)mgr,
                (unsigned long long)step, guard, (unsigned long long)au_modoff((void *)(uintptr_t)caller));
    } else if (idx == 10 && self > 0x1000) {
        /* 0x2df4b14 is the engine's per-tick render gate: it renders (vtable+0x70) only while this byte is set,
           and otherwise primes the sink with silence (vtable+0x78) — the flag is what the whole "no sound" hinges on. */
        uint64_t vt = *(uint64_t *)(uintptr_t)self;
        /* engine singleton -> event-queue object (+0x170); its +8 word is the pending-event count. A count that
           keeps growing means AK::SoundEngine::RenderAudio() (which drains the queue) is never running. */
        uint64_t *eng_slot = (uint64_t *)(uintptr_t)((uint8_t *)g_site_modbase + 0x430a120);
        uint64_t eng = *eng_slot, q = eng > 0x1000 ? *(uint64_t *)(uintptr_t)(eng + 0x170) : 0;
        unsigned qn = q > 0x1000 ? *(unsigned *)(uintptr_t)(q + 8) : 0;
        fprintf(stderr, "[site] %s #%d self=%#llx needrender=%u floatbuf=%#llx f0=%g f1=%g buf28=%#llx eng=%#llx qn=%u vt=%#llx m60=main+0x%llx m68=main+0x%llx m70=main+0x%llx m78=main+0x%llx caller main+0x%llx\n",
                g_site_tag[idx], n, (unsigned long long)self,
                (unsigned)*(uint8_t *)(uintptr_t)(self + 8),
                (unsigned long long)(*(uint64_t *)(uintptr_t)(self + 0x28)),
                *(float *)(uintptr_t)(*(uint64_t *)(uintptr_t)(self + 0x28)),
                *(float *)(uintptr_t)(*(uint64_t *)(uintptr_t)(self + 0x28) + 4),
                (unsigned long long)(*(uint64_t *)(uintptr_t)(self + 0x28)),
                (unsigned long long)eng, qn, (unsigned long long)vt,
                (unsigned long long)au_modoff((void *)(uintptr_t)(vt ? *(uint64_t *)(uintptr_t)(vt + 0x60) : 0)),
                (unsigned long long)au_modoff((void *)(uintptr_t)(vt ? *(uint64_t *)(uintptr_t)(vt + 0x68) : 0)),
                (unsigned long long)au_modoff((void *)(uintptr_t)(vt ? *(uint64_t *)(uintptr_t)(vt + 0x70) : 0)),
                (unsigned long long)au_modoff((void *)(uintptr_t)(vt ? *(uint64_t *)(uintptr_t)(vt + 0x78) : 0)),
                (unsigned long long)au_modoff((void *)(uintptr_t)caller));
    } else fprintf(stderr, "[site] %s #%d self=%#llx caller main+0x%llx\n", g_site_tag[idx], n,
            (unsigned long long)self, (unsigned long long)au_modoff((void *)(uintptr_t)caller));
    fflush(stderr);
}
__attribute__((used)) void *site0_stub_cont, *site1_stub_cont, *site2_stub_cont, *site3_stub_cont;
__attribute__((used)) void *site4_stub_cont, *site5_stub_cont, *site6_stub_cont, *site7_stub_cont;
__attribute__((used)) void *site8_stub_cont, *site9_stub_cont, *site10_stub_cont, *site11_stub_cont;
__attribute__((used)) void *site12_stub_cont, *site13_stub_cont, *site14_stub_cont, *site15_stub_cont;
#define AU_SITE_STUB(name, idx, w0, w1, w2)                                        \
    __attribute__((naked)) static void name(void) {                                \
        __asm__ volatile(                                                          \
            "stp x0, x1, [sp, #-16]!\n"                                            \
            "str x30, [sp, #-16]!\n"                                              \
            "mov x2, x30\n"                                                       \
            "mov x1, x0\n"                                                        \
            "mov w0, #" #idx "\n"                                                 \
            "bl _au_note_site\n"                                                  \
            "ldr x30, [sp], #16\n"                                                \
            "ldp x0, x1, [sp], #16\n"                                             \
            ".word " #w0 "\n" ".word " #w1 "\n" ".word " #w2 "\n"               \
            "adrp x16, _" #name "_cont@PAGE\n"                                      \
            "ldr x16, [x16, _" #name "_cont@PAGEOFF]\n"                             \
            "br x16\n");                                                           \
    }
AU_SITE_STUB(site0_stub, 0, 0xd10183ff, 0xa90357f6, 0xa9044ff4)
AU_SITE_STUB(site1_stub, 1, 0xd10483ff, 0xf9006bfc, 0xa90e5ff8)
AU_SITE_STUB(site2_stub, 2, 0xf81e0ff3, 0xa9017bfd, 0x910043fd)
AU_SITE_STUB(site3_stub, 3, 0xd100c3ff, 0xa9014ff4, 0xa9027bfd)
AU_SITE_STUB(site4_stub, 4, 0xf81e0ff3, 0xa9017bfd, 0x910043fd)
AU_SITE_STUB(site5_stub, 5, 0xf81e0ff3, 0xa9017bfd, 0x910043fd)
AU_SITE_STUB(site6_stub, 6, 0xa9be4ff4, 0xa9017bfd, 0x910043fd)
AU_SITE_STUB(site7_stub, 7, 0xd10303ff, 0xf90053f3, 0xa90b7bfd)
AU_SITE_STUB(site8_stub, 8, 0x52800020, 0xa9457bfd, 0xa9444ff4)
AU_SITE_STUB(site9_stub, 9, 0xa9bd57f6, 0xa9014ff4, 0xa9027bfd)
AU_SITE_STUB(site10_stub, 10, 0xf81e0ff3, 0xa9017bfd, 0x910043fd)
AU_SITE_STUB(site11_stub, 11, 0x6dbc23e9, 0xf9000bf5, 0xa9024ff4)
AU_SITE_STUB(site12_stub, 12, 0xf81e0ff3, 0xa9017bfd, 0x910043fd)
AU_SITE_STUB(site13_stub, 13, 0xd100c3ff, 0xa9014ff4, 0xa9027bfd)
AU_SITE_STUB(site14_stub, 14, 0xa9bb67fa, 0xa9015ff8, 0xa90257f6)
AU_SITE_STUB(site15_stub, 15, 0xa9bc5ff8, 0xa90157f6, 0xa9024ff4)
/* PLA_AUDIO_RENDER_DRIVE=1 (experiment): call the engine's RenderAudio entry (AK API slot +0x040 -> main+0x2d75fe0,
   which the game itself only calls once at boot from main+0x2781474) from the AK audio-device process function
   (main+0x2e16ef8, guest thread, ~44 times/s).  The entry takes a single bool (w0 = bAllowSyncRender, the dispatch
   thunk is "and w0,w1,#1; b 0x2d75fe0").  If RenderAudio is what drains the event queue into the mix, the float
   mixing buffer must stop being all-zero.  Env-gated, off by default. */
/* PLA_AUDIO_RENDER_TRACE=1 (diagnostic): count entries of main+0x2d75fe0 — the AK dispatch slot +0x040 impl, i.e.
   AK::SoundEngine::RenderAudio(bool) (thunk main+0x28372bc: "and w0,w1,#1; b 0x2d75fe0"). The game reads the AK
   dispatch table before any of our runtime hooks install, so a table-slot trampoline can miss cached calls; this
   hooks the impl itself, at load time, and answers "is the render stage actually being driven per frame". */
/* PLA_AUDIO_RENDER_TRACE=2 (diagnostic): also count main+0x2d7e250 — the inner render body RenderAudio calls when
   it is allowed to render synchronously — and dump the engine's render-side state words at entry (pending command
   count 0x430a248, suspended 0x430a5b8, device-list head 0x430a7e0, device count 0x430a7d0). */
__attribute__((used)) static void *render_body_cont;
static int g_rb_n;
void au_render_body_note(void);
void au_render_body_note(void) {
    int n = ++g_rb_n;
    if (n <= 3 || n % 500 == 0) {
        const char *e2 = getenv("PLA_AUDIO_RENDER_TRACE");
        uint8_t *b = (uint8_t *)g_site_modbase;
        unsigned qc = 0, sus = 0; uint64_t dl = 0, dc = 0;
        if (b && e2 && e2[0] == '2') {
            memcpy(&qc, b + 0x430a248, 4); sus = *(uint8_t *)(b + 0x430a5b8);
            memcpy(&dl, b + 0x430a7e0, 8); memcpy(&dc, b + 0x430a7d0, 4);
        }
        fprintf(stderr, "[rbody] inner render #%d qcount=%u suspended=%u dev_list=%#llx dev_count=%llu\n",
                n, qc, sus, (unsigned long long)dl, (unsigned long long)dc), fflush(stderr);
    }
}
__attribute__((naked)) static void render_body_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "bl _au_render_body_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        ".word 0xd101c3ff\n" ".word 0xa9016ffc\n" ".word 0xa90267fa\n"
        "adrp x16, _render_body_cont@PAGE\n"
        "ldr x16, [x16, _render_body_cont@PAGEOFF]\n"
        "br x16\n");
}
void runtime_audio_render_body_install(void);
void runtime_audio_render_body_install(void) {
    if (!getenv("PLA_AUDIO_RENDER_TRACE")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    if (!g_site_modbase) g_site_modbase = base;
    uint8_t *fn = base + 0x2d7e250;
    uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
    if (ex[0] != 0xd101c3ffu || ex[1] != 0xa9016ffcu || ex[2] != 0xa90267fau) {
        fprintf(stderr, "[rbody] prologue differs (%#x %#x %#x), skipped\n", ex[0], ex[1], ex[2]); return;
    }
    render_body_cont = fn + 12;
    uint32_t pt[3] = {0, 0, 0};
    emit_adrp_br(pt, (uintptr_t)fn, (void *)render_body_stub);
    memcpy(fn, pt, sizeof pt);
    sys_icache_invalidate(fn, 16);
    fprintf(stderr, "[rbody] inner render counter at main+0x2d7e250\n");
}
__attribute__((used)) static void *render_trace_cont;
static int g_rt_n;
/* PLA_SOUND_BOOTSTRAP=<stage>: bring the game's own sound subsystem up from the guest thread.
   1 = print the owner object only, 2 = also call the scene-sound factory (main+0x1dd9874),
   3 = also call the scene-sound init vtable+0x50 method (main+0x1dd7c68).  Zero-guest-perturbation at stage 1. */
/* PLA_DUMP_GO=1: once, print the engine's game-object table (engine+0x140: nbuckets@0x38, array@0x30,
   nodes {next@0, id@8, obj@0x10}) and the raw engine pointer. Event actions are dropped when the target id is
   missing from this table, so a table with zero buckets means no event can ever reach a voice. */
static void au_dump_go_table(void) {
    static int done; if (done) return; done = 1;
    uint8_t *mb = (uint8_t *)g_site_modbase; if (!mb) return;
    uint64_t eng = 0, tbl = 0; memcpy(&eng, mb + 0x430a120, 8);
    if (!AU_PTR_OK(eng)) { au_log_line("[godump] engine pointer %#llx is out of range\n", (unsigned long long)eng); return; }
    memcpy(&tbl, (void *)(uintptr_t)(eng + 0x140), 8);
    if (!AU_PTR_OK(tbl)) { au_log_line("[godump] engine=%#llx table pointer %#llx is out of range\n",
                                       (unsigned long long)eng, (unsigned long long)tbl); return; }
    unsigned nb = 0, nn2 = 0; uint64_t arr = 0;
    if (tbl) {
        memcpy(&nb, (void *)(uintptr_t)(tbl + 0x38), 4);
        memcpy(&arr, (void *)(uintptr_t)(tbl + 0x30), 8);
        if (AU_PTR_OK(arr) && nb && nb <= 4096)
            for (unsigned i = 0; i < nb && nn2 < 512; i++) {
                uint64_t h = 0; memcpy(&h, (void *)(uintptr_t)(arr + 8 * i), 8);
                int guard = 0;
                while (h && nn2 < 512 && guard++ < 256) { uint64_t nx = 0; memcpy(&nx, (void *)(uintptr_t)h, 8); nn2++; h = nx; }
            }
    }
    au_log_line("[godump] engine=%#llx table=%#llx nbuckets=%u objects=%u\n",
                (unsigned long long)eng, (unsigned long long)tbl, nb, nn2);
}
void au_sound_bootstrap_run(void);
void au_sound_bootstrap_run(void) {
    static int done;
    const char *e = getenv("PLA_SOUND_BOOTSTRAP");
    if (!e || done || !g_site_modbase) return;
    int stage = atoi(e);
    if (stage <= 0) return;
    if (g_rt_n < 120) return;                    /* let the boot settle before touching guest objects */
    uint8_t *base = (uint8_t *)g_site_modbase;
    uint64_t g160 = 0, o1 = 0, owner = 0, vt = 0;
    memcpy(&g160, base + 0x427b160, 8);
    if (g160) memcpy(&o1, (void *)(uintptr_t)g160, 8);
    if (o1) memcpy(&owner, (void *)(uintptr_t)(o1 + 0x388), 8);
    if (owner) memcpy(&vt, (void *)(uintptr_t)owner, 8);
    fprintf(stderr, "[sndboot] frame=%d g160=%#llx o1=%#llx owner=%#llx owner_vt=%#llx stage=%d\n",
            g_rt_n, (unsigned long long)g160, (unsigned long long)o1,
            (unsigned long long)owner, (unsigned long long)vt, stage);
    fflush(stderr);
    if (owner && stage >= 2) {
        /* replicate the real call: 0x1dd7410 loads the second argument as [ *[base+0x4279680] + 0xc8 ] */
        uint64_t p680 = 0, v680 = 0, x21 = 0;
        memcpy(&p680, base + 0x4279680, 8);
        if (p680) memcpy(&v680, (void *)(uintptr_t)p680, 8);
        if (v680) memcpy(&x21, (void *)(uintptr_t)(v680 + 0xc8), 8);
        fprintf(stderr, "[sndboot] second arg: p680=%#llx v680=%#llx x21=%#llx\n",
                (unsigned long long)p680, (unsigned long long)v680, (unsigned long long)x21); fflush(stderr);
        void *(*factory)(void *, void *) = (void *(*)(void *, void *))(void *)(base + 0x1dd9874);
        if (!x21) { fprintf(stderr, "[sndboot] no second arg, factory skipped\n"); fflush(stderr); return; }
        void *obj = factory((void *)(uintptr_t)owner, (void *)(uintptr_t)x21);
        fprintf(stderr, "[sndboot] factory(main+0x1dd9874) -> %p\n", obj); fflush(stderr);
        if (obj) {
            uint64_t w[6]; memcpy(w, obj, sizeof w);
            fprintf(stderr, "[sndboot] obj vt=%#llx(expect main+0x4158a18=%#llx) +8=%#llx +0x10=%#llx +0x18=%#llx +0x20=%#llx +0x58=%#llx\n",
                    (unsigned long long)w[0], (unsigned long long)(uintptr_t)(base + 0x4158a18),
                    (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3],
                    (unsigned long long)w[4], (unsigned long long)w[5]);
            fflush(stderr);
        }
        if (obj && stage >= 3) {
            void (*scinit)(void *) = (void (*)(void *))(void *)(base + 0x1dd7c68);
            scinit(obj);
            fprintf(stderr, "[sndboot] scene sound init (main+0x1dd7c68) returned\n"); fflush(stderr);
        }
    }
    done = 1;
}
/* PLA_REG_GO_RT=<frame>[,<go>...]: once, at the given RenderAudio tick (i.e. on a guest thread, with the engine
   already running), feed the AK command that registers a game object.  The engine drops every event whose target
   id is missing from the game-object table at [base+0x430a140], so this is the cheapest way to find out whether
   registering the id the game already uses (1) is enough to make events reach a voice. */
unsigned au_ring_pend(uint8_t *mb) {
    uint64_t q = 0; memcpy(&q, mb + 0x430a128, 8);
    if (!AU_PTR_OK(q)) return 0xffffffffu;
    uint64_t rd = 0, wr = 0, cap = 0;
    memcpy(&rd, (void *)(uintptr_t)q, 8);
    memcpy(&wr, (void *)(uintptr_t)(q + 8), 8);
    memcpy(&cap, (void *)(uintptr_t)(q + 0x20), 8);
    if (!cap) return 0xffffffffu;
    return (unsigned)(wr >= rd ? (wr - rd) : (wr - rd + cap));
}

/* Snapshot of the pending-action hash map (global [base+0x430a170], see 0x2ddcc24).  Called from the PostEvent
   probe right after an event is accepted, so "queued but never executed" becomes a number instead of an inference. */
static void au_dump_actionmap(const char *tag) {
    uint8_t *mb = (uint8_t *)g_site_modbase;
    if (!mb) return;
    uint64_t jm = 0; memcpy(&jm, mb + 0x430a170, 8);
    if (!AU_PTR_OK(jm)) { au_log_line("[actmap] %s: jobmgr insane (%#llx)\n", tag, (unsigned long long)jm); return; }
    uint64_t arr = 0; uint32_t nbuck = 0, live = 0, lf = 0;
    memcpy(&arr, (void *)(uintptr_t)jm, 8);
    memcpy(&nbuck, (void *)(uintptr_t)(jm + 8), 4);
    memcpy(&live, (void *)(uintptr_t)(jm + 0x10), 4);
    memcpy(&lf, (void *)(uintptr_t)(jm + 0x18), 4);
    unsigned walk = 0;
    if (AU_PTR_OK(arr) && nbuck && nbuck < 65536) {
        for (unsigned i = 0; i < nbuck && walk < 2048; i++) {
            uint64_t h = 0; memcpy(&h, (void *)(uintptr_t)(arr + (uint64_t)i * 8), 8);
            for (int d = 0; d < 64 && AU_PTR_OK(h); d++) {
                uint64_t nx = 0; memcpy(&nx, (void *)(uintptr_t)h, 8); h = nx; walk++; } } }
    au_log_line("[actmap] %s buckets=%u live=%u lf=%u entries=%u\n", tag, nbuck, live, lf, walk);
}

static void au_register_go_runtime(void) {
    static int done, check_at, check_pending;
    uint8_t *mb = (uint8_t *)g_site_modbase;
    if (!mb) return;
    if (check_pending && g_rt_n >= check_at) {
        check_pending = 0;
        uint64_t tbl = 0; memcpy(&tbl, mb + 0x430a140, 8);
        unsigned b = 0, cmd = 0, objs = 0; int found1 = 0; unsigned ids[6]; unsigned nid = 0;
        if (AU_PTR_OK(tbl)) memcpy(&b, (void *)(uintptr_t)(tbl + 0x38), 4);
        memcpy(&cmd, mb + 0x430a248, 4);
        uint64_t arr = 0;
        if (AU_PTR_OK(tbl)) memcpy(&arr, (void *)(uintptr_t)(tbl + 0x30), 8);
        if (AU_PTR_OK(arr) && b && b < 65536) {
            for (unsigned i = 0; i < b; i++) {
                uint64_t h = 0; memcpy(&h, (void *)(uintptr_t)(arr + 8 * i), 8);
                int guard = 0;
                while (AU_PTR_OK(h) && guard++ < 256) {
                    uint64_t id = 0, nx = 0;
                    memcpy(&id, (void *)(uintptr_t)(h + 8), 8);
                    memcpy(&nx, (void *)(uintptr_t)h, 8);
                    objs++; if (id == 1) found1 = 1;
                    if (nid < 6) ids[nid++] = (unsigned)id;
                    h = nx;
                }
            }
        }
        au_log_line("[reggort] +%d ticks later: buckets=%u objs=%u found_go1=%d ids=%u,%u,%u,%u,%u,%u cmd=%u pend=%u\n",
                    g_rt_n - check_at + 200, b, objs, found1,
                    nid > 0 ? ids[0] : 0, nid > 1 ? ids[1] : 0, nid > 2 ? ids[2] : 0,
                    nid > 3 ? ids[3] : 0, nid > 4 ? ids[4] : 0, nid > 5 ? ids[5] : 0,
                    cmd, au_ring_pend(mb));
        return;
    }
    if (done) return;
    const char *spec = getenv("PLA_REG_GO_RT");
    if (!spec || !*spec) return;
    if (g_rt_n < 1200) return;   /* wait for the engine + UI to be up (~27 s of frames) */
    done = 1;
    uint64_t tbl = 0; memcpy(&tbl, mb + 0x430a140, 8);
    unsigned before = 0;
    if (AU_PTR_OK(tbl)) memcpy(&before, (void *)(uintptr_t)(tbl + 0x38), 4);
    unsigned pend_before = au_ring_pend(mb);
    int (*reg)(uint64_t, uint64_t, uint64_t) = (int (*)(uint64_t, uint64_t, uint64_t))(mb + 0x2837328ULL);
    char buf[256]; snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        uint64_t go = strtoull(t, NULL, 0);
        static char gname[64];
        snprintf(gname, sizeof gname, "pla_go_%llu", (unsigned long long)go);
        int r = reg(0, go, (uint64_t)(uintptr_t)gname);
        au_log_line("[reggort] n=%d slot+0x050(go=%llu) -> %d buckets_before=%u pend_before=%u pend_after=%u\n",
                    g_rt_n, (unsigned long long)go, r, before, pend_before, au_ring_pend(mb));
    }
    check_at = g_rt_n + 200; check_pending = 1;
}
void au_render_trace_note(uint64_t caller);
static void au_test_play_note(void);
void au_render_trace_note(uint64_t caller) {
    au_test_play_note();
    au_sound_bootstrap_run();
    int n = ++g_rt_n;
    au_register_go_runtime();
    if (getenv("PLA_DUMP_PLAYQ") && ((n % 16) == 0 || n <= 6)) {
        static uint32_t q_max;
        uint64_t holder = 0;
        if (!g_site_modbase) { au_log_line("[playq] n=%d modbase=0\n", n); return; }
        memcpy(&holder, (void *)(uintptr_t)(g_site_modbase + 0x430a150), 8);
        if (AU_PTR_OK(holder)) {
            uint32_t cnt = 0, cap = 0; uint64_t vec = 0;
            memcpy(&vec, (void *)(uintptr_t)holder, 8);
            memcpy(&cnt, (void *)(uintptr_t)(holder + 8), 4);
            memcpy(&cap, (void *)(uintptr_t)(holder + 0xc), 4);
            if (cnt > q_max) q_max = cnt;
            if (n == 64) {
                uint64_t hvt = 0, vec1 = 0; uint32_t c1 = 0, p1 = 0;
                memcpy(&hvt, (void *)(uintptr_t)holder, 8);
                memcpy(&vec1, (void *)(uintptr_t)(holder + 0x10), 8);
                memcpy(&c1, (void *)(uintptr_t)(holder + 0x18), 4);
                memcpy(&p1, (void *)(uintptr_t)(holder + 0x1c), 4);
                au_log_line("[playq] holder vt=main+0x%llx vec1=%#llx cnt1=%u cap1=%u\n",
                            (unsigned long long)au_modoff((void *)(uintptr_t)hvt),
                            (unsigned long long)vec1, c1, p1);
                for (int i = 0; i < 16 && AU_PTR_OK(hvt); i++) {
                    uint64_t sl = 0; memcpy(&sl, (void *)(uintptr_t)(hvt + 8 * i), 8);
                    if (!sl) continue;
                    au_log_line("[playq]   vt+%#03x = main+0x%llx\n", 8 * i,
                                (unsigned long long)au_modoff((void *)(uintptr_t)sl));
                }
            }
            if (cnt || q_max || (n % 64) == 0)
                au_log_line("[playq] n=%d holder=%#llx vec=%#llx cnt=%u max=%u cap=%u\n",
                            n, (unsigned long long)holder, (unsigned long long)vec,
                            cnt, q_max, cap);
        } else {
            au_log_line("[playq] n=%d holder=%#llx BAD\n", n, (unsigned long long)holder);
        }
    }
    if (n <= 6 || n % 2000 == 0)
        fprintf(stderr, "[rtrace] RenderAudio #%d caller main+0x%llx\n", n,
                (unsigned long long)au_modoff((void *)(uintptr_t)caller)), fflush(stderr);
}
__attribute__((naked)) static void render_trace_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x0, x30\n"
        "bl _au_render_trace_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        ".word 0xf81b0ff9\n" ".word 0xa9015ff8\n" ".word 0xa90257f6\n"
        "adrp x16, _render_trace_cont@PAGE\n"
        "ldr x16, [x16, _render_trace_cont@PAGEOFF]\n"
        "br x16\n");
}
/* PLA_FORCE_GLOBAL_GO=1: the game posts every event for game object 1, but no game object is ever registered,
   so the message dispatcher drops each one at the empty game-object table.  Forcing the target id to
   AK_INVALID_GAME_OBJECT ( -1 ) takes the global branch at main+0x2d7ed1c, which resolves the action directly.
   This is a diagnostic: it tells us whether the rest of the chain (voice creation + mixing) works once the drop is
   removed, and, if it does, whether the sound is audibly correct as 2D playback. */
/* PLA_GO_GLOBAL_FALLBACK=1: diagnostic/behaviour patch.  The event action dispatcher drops every message whose
   target game object is missing from the game-object table (main+0x2d7ec7c: cbz w10 -> "message done").  This
   patch turns that drop into the same path a global event (id == -1) takes at main+0x2d7ed1c, which resolves and
   executes the action without consulting the table.  Positional game objects therefore fall back to 2D playback. */
void runtime_audio_go_fallback_install(void);
void runtime_audio_go_fallback_install(void) {
    if (!getenv("PLA_GO_GLOBAL_FALLBACK")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    uint32_t *at = (uint32_t *)(base + 0x2d7ec7cULL);
    if (*at != 0x34fffbeaU) {
        fprintf(stderr, "[gofb] expected cbz w10 at main+0x2d7ec7c, found %#x, skipped\n", *at);
        fflush(stderr); return;
    }
    uint32_t b = 0x14000028U;                       /* b #+0xa0 -> main+0x2d7ed1c (global action path) */
    memcpy(at, &b, 4);
    sys_icache_invalidate(at, 4);
    fprintf(stderr, "[gofb] empty game-object table now falls back to the global action path\n");
    fflush(stderr);
}

void runtime_audio_force_global_go_install(void);
void runtime_audio_force_global_go_install(void) {
    if (!getenv("PLA_FORCE_GLOBAL_GO")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    uint32_t *at = (uint32_t *)(base + 0x2813478ULL);           /* ldr x1, [x22, #8]  -- the game object id */
    if ((*at & 0xffc00000u) != 0xf9400000u || (*at & 31u) != 1u) {   /* LDR (imm, 64-bit) into x1 */
        fprintf(stderr, "[globalgo] expected 'ldr x1,[x22,#8]' at main+0x2813478, found %#x, skipped\n", *at);
        fflush(stderr); return;
    }
    uint32_t movn = 0x92800001u;                                 /* movn x1, #0  ==  -1 (AK_INVALID_GAME_OBJECT) */
    memcpy(at, &movn, 4);
    sys_icache_invalidate(at, 4);
    fprintf(stderr, "[globalgo] PostEvent target id forced to AK_INVALID_GAME_OBJECT\n");
    fflush(stderr);
}

void runtime_audio_render_trace_install(void);
void runtime_audio_render_trace_install(void) {
    if (!getenv("PLA_AUDIO_RENDER_TRACE") && !getenv("PLA_SOUND_BOOTSTRAP") && !getenv("PLA_INJECT_EVENT") &&
        !getenv("PLA_REG_GO_RT") && !getenv("PLA_DUMP_PLAYQ")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    if (!g_site_modbase) g_site_modbase = base;
    uint8_t *fn = base + 0x2d75fe0;
    uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
    if (ex[0] != 0xf81b0ff9u || ex[1] != 0xa9015ff8u || ex[2] != 0xa90257f6u) {
        fprintf(stderr, "[rtrace] prologue differs (%#x %#x %#x), skipped\n", ex[0], ex[1], ex[2]); return;
    }
    render_trace_cont = fn + 12;
    uint32_t pt[3] = {0, 0, 0};
    emit_adrp_br(pt, (uintptr_t)fn, (void *)render_trace_stub);
    memcpy(fn, pt, sizeof pt);
    sys_icache_invalidate(fn, 16);
    fprintf(stderr, "[rtrace] RenderAudio counter at main+0x2d75fe0\n");
}
/* PLA_AUDIO_CMD_PROBE=1 (diagnostic): count three points of the AK command/event pipeline so we can tell where a
   posted event stops.  Sites (module offsets, 3-word non-PC-relative prologues only):
     main+0x2ddcc24  "evpost"  event-manager enqueue (called from PostEvent once the event table lookup succeeds)
     main+0x2d7de5c  "alloc"   AK command-pool allocator (a=pool ptr, b=total byte size rounded up)
     main+0x2dc98a8  "nocmd"   render body path taken when the pending-command counter is <= 0
   Env-gated, off by default; installed at load time (text is still writable) from
   runtime_audio_bank_bootstrap_install(). */
__attribute__((used)) static void *cmd_evpost_stub_cont;
__attribute__((used)) static void *cmd_alloc_stub_cont;
__attribute__((used)) static void *cmd_nocmd_stub_cont;
__attribute__((used)) static void *cmd_ring_stub_cont;
__attribute__((used)) static void *cmd_msgdone_stub_cont;
__attribute__((used)) static void *cmd_goevent_stub_cont;
__attribute__((used)) static void *cmd_gofound_stub_cont;
__attribute__((used)) static void *cmd_action_stub_cont;
__attribute__((used)) static void *cmd_reggo_stub_cont;
__attribute__((used)) static void *cmd_unreggo_stub_cont;
__attribute__((used)) static void *cmd_factory_stub_cont;
__attribute__((used)) static void *cmd_sceneinit_stub_cont;
__attribute__((used)) static void *chain_sceneupd_stub_cont;
__attribute__((used)) static void *chain_ch1_stub_cont;
__attribute__((used)) static void *chain_ch2_stub_cont;
__attribute__((used)) static void *chain_ch3_stub_cont;
__attribute__((used)) static void *chain_factory2_stub_cont;
__attribute__((used)) static void *chain_scinit_stub_cont;
__attribute__((used)) static void *chain_famctor_stub_cont;
__attribute__((used)) static void *chain_famsib_stub_cont;
__attribute__((used)) static void *chain_famupd_stub_cont;
__attribute__((used)) static void *chain_postev_stub_cont;
__attribute__((used)) static void *chain_usercmd_stub_cont;
__attribute__((used)) static void *chain_reggo_stub_cont;    /* API slot +0x048 post-validation (register family) */   /* AK PostEvent(eventID) impl, entry 0x2d76f2c */
__attribute__((used)) static void *mixdev_stub_cont;          /* engine per-device mixer pass (calls sink vtable+0x48) */
__attribute__((used)) static void *voicecreate_stub_cont;     /* voice pool append */
__attribute__((used)) static void *vcbuild_stub_cont;       /* voice build/prepare (upstream of the pool append) */
__attribute__((used)) static void *vccaller1_stub_cont;     /* one of the two functions calling voice create */
__attribute__((used)) static void *vccaller2_stub_cont;
__attribute__((used)) static void *chain_sndtop_stub_cont;
__attribute__((used)) static void *chain_sndbig_stub_cont;
__attribute__((used)) static void *chain_ready_stub_cont;
__attribute__((used)) static void *chain_st1_stub_cont;
__attribute__((used)) static void *chain_st2_stub_cont;
__attribute__((used)) static void *chain_st3_stub_cont;
static int g_cmd_n[64];
static void au_log_line(const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n > 0) { size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1; ssize_t w = write(2, buf, len); (void)w; }
}
void au_cmd_note(uint64_t idx, uint64_t a, uint64_t b, uint64_t caller);
void au_cmd_note(uint64_t idx, uint64_t a, uint64_t b, uint64_t caller) {
    if (idx >= 64) return;
    int n = ++g_cmd_n[idx];
    if (idx == 3) {                       /* message ring: a = ring object, [0]=read [8]=write [0x20]=cap */
        uint64_t rd = 0, wr = 0, cap = 0; int pend = -1;
        if (a) { memcpy(&rd, (void *)(uintptr_t)a, 8); memcpy(&wr, (void *)(uintptr_t)(a + 8), 8);
                 memcpy(&cap, (void *)(uintptr_t)(a + 0x20), 8);
                 pend = (int)(wr >= rd ? (wr - rd) : (wr - rd + cap)); }
        unsigned type = 0; if (rd) { uint16_t t = 0; memcpy(&t, (void *)(uintptr_t)(rd + 2), 2); type = t; }
        static int last_pend = -2; static int changes = 0;
        if (pend != last_pend && changes < 40) { last_pend = pend; changes++; }
        else if (n > 3) { return; }
        {
            uint8_t *mb = (uint8_t *)g_site_modbase;
            uint64_t gotbl = 0, eng = 0, gobjtbl = 0; unsigned nn = 0;
            if (mb) {
                memcpy(&nn, mb + 0x41ab268, 4);
                memcpy(&eng, mb + 0x430a120, 8);
                memcpy(&gotbl, mb + 0x430a140, 8);
            }
            unsigned nb = 0, nn2 = 0; uint64_t barr = 0;
            if (gotbl) {
                memcpy(&nb, (void *)(uintptr_t)(gotbl + 0x38), 4);
                memcpy(&barr, (void *)(uintptr_t)(gotbl + 0x30), 8);
                if (barr && nb && nb <= 4096)
                    for (unsigned i = 0; i < nb && nn2 < 128; i++) {
                        uint64_t h = 0; memcpy(&h, (void *)(uintptr_t)(barr + 8 * i), 8);
                        int guard = 0;
                        while (h && nn2 < 128 && guard++ < 128) { uint64_t nx = 0; memcpy(&nx, (void *)(uintptr_t)h, 8); nn2++; h = nx; }
                    }
            }
            (void)gobjtbl;
            au_log_line("[cmd] ring #%d pend=%d type=%#x modbase=%#llx N=%u eng=%#llx gotbl=%#llx gobjbuckets=%u gobjs=%u rd=%#llx wr=%#llx\n",
                    n, pend, type, (unsigned long long)(uintptr_t)mb, nn,
                    (unsigned long long)eng, (unsigned long long)gotbl, nb, nn2,
                    (unsigned long long)rd, (unsigned long long)wr);
        }
        return;
    }
    if (idx == 12 || idx == 13) {
        if (n <= 4 || n % 100 == 0)
            au_log_line("[cmd] %s #%d obj=%#llx caller main+0x%llx\n", idx == 12 ? "sndfactory" : "sndsceneinit", n,
                    (unsigned long long)a, (unsigned long long)au_modoff((void *)(uintptr_t)caller));
        return;
    }
    if (idx == 10 || idx == 11) {
        if (n <= 4 || n % 200 == 0)
            au_log_line("[cmd] %s #%d go=%#llx caller main+0x%llx\n", idx == 10 ? "reggo50" : "reggo58", n,
                    (unsigned long long)a, (unsigned long long)au_modoff((void *)(uintptr_t)caller));
        return;
    }
    if (idx == 7) {
        unsigned g8 = 0; if (a) memcpy(&g8, (void *)(uintptr_t)(a + 8), 4);
        if (n <= 6 || n % 200 == 0)
            au_log_line("[cmd] action #%d mgr=%#llx gate8=%u caller main+0x%llx\n", n,
                    (unsigned long long)a, g8, (unsigned long long)au_modoff((void *)(uintptr_t)caller)),
                    fflush(stderr);
        return;
    }
    if (idx == 42) {                       /* API slot +0x048: does the game ever register a game object? */
        if (n <= 40 || (n % 50) == 0)
            au_log_line("[sndchain] reggo #%d go=%#llx caller main+0x%llx\n",
                        n, (unsigned long long)b, (unsigned long long)au_modoff((void *)(uintptr_t)caller));
        return;
    }
    if (idx == 41) {                       /* AK PostEvent inline body: which game object does the game target? */
        uint8_t *mb = (uint8_t *)g_site_modbase; unsigned nb = 0;
        uint64_t eng = 0, tbl = 0;
        /* The dispatcher passes [base+0x430a140] as the game-object-table object to main+0x2df2694 and
           main+0x2d7ec5c; its bucket count lives at +0x38. This is NOT engine+0x140. */
        (void)eng;
        if (mb) { memcpy(&tbl, mb + 0x430a140, 8);
                  if (AU_PTR_OK(tbl)) memcpy(&nb, (void *)(uintptr_t)(tbl + 0x38), 4); }
        if (n <= 40 || (n % 50) == 0)
            au_log_line("[sndchain] usercmd #%d go=%#llx a2=%#llx gobuckets=%u caller main+0x%llx\n",
                        n, (unsigned long long)b, (unsigned long long)a, nb,
                        (unsigned long long)au_modoff((void *)(uintptr_t)caller));
        return;
    }
    if (idx == 35) {                       /* AK::SoundEngine::PostEvent impl: does the event target a registered game object? */
        uint8_t *mb = (uint8_t *)g_site_modbase; unsigned nb = 0; unsigned found = 0, nobjs = 0;
        /* The very first PostEvent can happen before the engine object exists; every deref must be range
           checked, otherwise a bogus pointer faults inside the guest (SIGBUS on a read-only page). */
        /* The dispatch handlers receive the table OBJECT from `[base+0x430a140]`, not `engine+0x140`
           (the latter holds a poison value). Read the same object here so the bucket count is comparable. */
        if (mb) { uint64_t tbl = 0, arr = 0;
                  memcpy(&tbl, mb + 0x430a140, 8);
                  if (AU_PTR_OK(tbl)) { memcpy(&nb, (void *)(uintptr_t)(tbl + 0x38), 4);
                             if (nb && nb <= 4096) { memcpy(&arr, (void *)(uintptr_t)(tbl + 0x30), 8);
                               if (AU_PTR_OK(arr)) {
                                 /* walk every bucket chain: count live objects and look for the event's target */
                                 for (unsigned h = 0; h < nb; h++) {
                                   uint64_t node = 0;
                                   memcpy(&node, (void *)(uintptr_t)(arr + (uint64_t)h * 8), 8);
                                   for (unsigned d = 0; d < 64 && AU_PTR_OK(node); d++) {
                                     uint64_t nid = 0, nxt = 0;
                                     memcpy(&nid, (void *)(uintptr_t)(node + 8), 8);
                                     memcpy(&nxt, (void *)(uintptr_t)node, 8);
                                     nobjs++; if (nid == b) found = 1;
                                     node = nxt; } } } } } }
        {   char tag[48]; snprintf(tag, sizeof tag, "after postevent #%d", n); au_dump_actionmap(tag); }
        if (n <= 40 || (n % 50) == 0)
            au_log_line("[sndchain] postevent #%d eventID=%#llx go=%#llx gobuckets=%u gobjects=%u gofound=%u caller main+0x%llx\n",
                        n, (unsigned long long)a, (unsigned long long)b, nb, nobjs, found,
                        (unsigned long long)au_modoff((void *)(uintptr_t)caller));
        return;
    }
    if (idx >= 45 && idx <= 49) {          /* play-request object interface methods (vtable main+0x41aaa60) */
        static const char *nm[8] = {"req.m20", "req.m40", "req.m60", "req.m80", "req.ma0", "?", "?", "?"};
        uint8_t *mb = (uint8_t *)g_site_modbase;
        uint64_t eng = 0, vt = 0, tgt = 0, rvt = 0, f0 = 0, f8 = 0;
        unsigned nb = 0;
        if (mb) {
            memcpy(&eng, mb + 0x42f1798, 8);            /* engine singleton (non-TLS fallback slot) */
            if (AU_PTR_OK(eng)) {
                memcpy(&vt, (void *)(uintptr_t)eng, 8);
                if (AU_PTR_OK(vt)) memcpy(&tgt, (void *)(uintptr_t)(vt + 0xc0), 8);
            }
            memcpy(&nb, mb + 0x41ab268, 4);
        }
        if (AU_PTR_OK(a)) {
            memcpy(&rvt, (void *)(uintptr_t)a, 8);
            memcpy(&f0, (void *)(uintptr_t)(a + 0x18), 8);
            memcpy(&f8, (void *)(uintptr_t)(a + 0x20), 8);
        }
        if (n <= 4 || (n % 20) == 0)
            au_log_line("[req] %s #%d this=%#llx rvt=main+0x%llx f18=%#llx f20=%#llx N=%u eng=%#llx engvt=main+0x%llx vt_c0=main+0x%llx caller main+0x%llx\n",
                        nm[idx - 45], n, (unsigned long long)a,
                        (unsigned long long)au_modoff((void *)(uintptr_t)rvt), (unsigned long long)f0,
                        (unsigned long long)f8, nb, (unsigned long long)eng,
                        (unsigned long long)au_modoff((void *)(uintptr_t)vt),
                        (unsigned long long)au_modoff((void *)(uintptr_t)tgt),
                        (unsigned long long)au_modoff((void *)(uintptr_t)caller));
        return;
    }
    if (idx >= 20 && idx <= 34) {          /* game-side sound-object construction chain */
        static const char *names[16] = {"sceneupd", "ch1", "ch2", "ch3", "factory", "scinit",
                                       "famctor", "famsib", "famupd", "sndtop", "sndbig", "ready",
                                       "appst1", "appst2", "appst3", "postevent"};
        uint8_t *mb = (uint8_t *)g_site_modbase;
        if (n <= 6 || n % 500 == 0) {
            uint64_t f498 = 0, f4a0 = 0, f4a8 = 0, f4d0 = 0, f520 = 0, f528 = 0, owner = 0, g160 = 0;
            unsigned b498 = 0xff, b520 = 0xff;
            if (mb) {
                memcpy(&f498, mb + 0x4279000 + 0x498, 8);
                memcpy(&f4a0, mb + 0x4279000 + 0x4a0, 8);
                memcpy(&f4a8, mb + 0x4279000 + 0x4a8, 8);
                memcpy(&f4d0, mb + 0x4279000 + 0x4d0, 8);
                memcpy(&f520, mb + 0x4279000 + 0x520, 8);
                memcpy(&f528, mb + 0x4279000 + 0x528, 8);
                memcpy(&g160, mb + 0x427b000 + 0x160, 8);
                if (f498) memcpy(&b498, (void *)(uintptr_t)f498, 1);
                if (f520) memcpy(&b520, (void *)(uintptr_t)f520, 1);
                if (g160) {
                    uint64_t h = 0; memcpy(&h, (void *)(uintptr_t)g160, 8);
                    if (h) memcpy(&owner, (void *)(uintptr_t)(h + 0x388), 8);
                }
            }
            uint64_t lazy = 0;
            if (idx == 25 && a) memcpy(&lazy, (void *)(uintptr_t)(a + 0x88), 8);
            au_log_line("[sndchain] %s #%d self=%#llx x2=%#llx caller main+0x%llx lazy88=%#llx "
                            "f498=%#llx b498=%u f4a0=%#llx f4a8=%#llx f4d0=%#llx f520=%#llx b520=%u f528=%#llx g160=%#llx owner=%#llx\n",
                    names[idx - 20], n, (unsigned long long)a, (unsigned long long)b,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller),
                    (unsigned long long)lazy, (unsigned long long)f498, b498,
                    (unsigned long long)f4a0, (unsigned long long)f4a8, (unsigned long long)f4d0,
                    (unsigned long long)f520, b520, (unsigned long long)f528,
                    (unsigned long long)g160, (unsigned long long)owner);
        }
        return;
    }
    if (idx == 53 || idx == 54) {       /* the only two functions that call AK voice creation */
        uint8_t *mb = (uint8_t *)g_site_modbase;
        unsigned vcnt = 0; if (mb) memcpy(&vcnt, mb + 0x430ae08, 4);
        if (n <= 8 || (n % 100) == 0)
            au_log_line("[mixer] %s #%d self=%#llx x1=%#llx voices=%u caller main+0x%llx\n",
                    idx == 53 ? "vccaller1" : "vccaller2", n, (unsigned long long)a, (unsigned long long)b, vcnt,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
        return;
    }
    if (idx == 52) {                    /* voice build: are we even reaching voice creation? */
        uint8_t *mb = (uint8_t *)g_site_modbase;
        unsigned vcnt = 0; if (mb) memcpy(&vcnt, mb + 0x430ae08, 4);
        if (n <= 8 || (n % 200) == 0)
            au_log_line("[mixer] vcbuild #%d self=%#llx x1=%#llx voices=%u caller main+0x%llx\n",
                    n, (unsigned long long)a, (unsigned long long)b, vcnt,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
        return;
    }
    if (idx == 50 || idx == 51) {         /* engine mix pass entry / voice creation: why does the mixer return early? */
        uint8_t *mb = (uint8_t *)g_site_modbase;
        uint64_t varr = 0, ch = 0; unsigned vcnt = 0; uint8_t g44 = 0xff, g08 = 0xff;
        if (mb) {
            memcpy(&varr, mb + 0x430ae00, 8);
            memcpy(&vcnt, mb + 0x430ae08, 4);
            memcpy(&g08, mb + 0x430ae08, 1);
            memcpy(&g44, mb + 0x430ae44, 1);
            memcpy(&ch, mb + 0x430ae08, 8);
        }
        (void)ch;
        if (n <= 8 || (n % 400) == 0)
            au_log_line("[mixer] %s #%d a0=%#llx voices=%u varr=%#llx gate08=%u gate44=%u caller main+0x%llx\n",
                    idx == 50 ? "mixdev" : "voicecreate", n, (unsigned long long)a, vcnt,
                    (unsigned long long)varr, g08, g44,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
        return;
    }
    if (idx == 1 ? (n <= 300) : (n <= 6 || n % 400 == 0))
        au_log_line("[cmd] %s #%d a=%#llx b=%llu caller main+0x%llx\n",
                idx == 0 ? "evpost" : idx == 1 ? "alloc" : idx == 2 ? "nocmd" :
                idx == 4 ? "msgdone" : idx == 5 ? "goevent" : idx == 6 ? "gofound" : "x",
                n, (unsigned long long)a, (unsigned long long)b,
                (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
/* Same idea as AU_CMD_STUB but also preserves q0..q3: the AK action/voice entry points take float
   arguments in d0..d3, and the C logging call would clobber them. */
#define AU_CMD_STUB_Q(name, idx, w0, w1, w2)                                        \
    __attribute__((naked)) static void name(void) {                                 \
        __asm__ volatile(                                                           \
            "sub sp, sp, #160\n"                                                    \
            "stp x0, x1, [sp, #0]\n"                                                \
            "stp x2, x3, [sp, #16]\n"                                               \
            "stp x4, x5, [sp, #32]\n"                                               \
            "stp x6, x7, [sp, #48]\n"                                               \
            "stp q0, q1, [sp, #64]\n"                                               \
            "stp q2, q3, [sp, #96]\n"                                               \
            "str x30, [sp, #128]\n"                                                 \
            "mov x3, x30\n"                                                         \
            "ldr x2, [sp, #8]\n"                                                    \
            "ldr x1, [sp, #0]\n"                                                    \
            "mov w0, #" #idx "\n"                                                   \
            "bl _au_cmd_note\n"                                                     \
            "ldr x30, [sp, #128]\n"                                                 \
            "ldp q2, q3, [sp, #96]\n"                                               \
            "ldp q0, q1, [sp, #64]\n"                                               \
            "ldp x6, x7, [sp, #48]\n"                                               \
            "ldp x4, x5, [sp, #32]\n"                                               \
            "ldp x2, x3, [sp, #16]\n"                                               \
            "ldp x0, x1, [sp, #0]\n"                                                \
            "add sp, sp, #160\n"                                                    \
            ".word " #w0 "\n" ".word " #w1 "\n" ".word " #w2 "\n"                   \
            "adrp x16, _" #name "_cont@PAGE\n"                                       \
            "ldr x16, [x16, _" #name "_cont@PAGEOFF]\n"                              \
            "br x16\n");                                                             \
    }

#define AU_CMD_STUB(name, idx, w0, w1, w2)                                          \
    __attribute__((naked)) static void name(void) {                                 \
        __asm__ volatile(                                                           \
            "stp x0, x1, [sp, #-16]!\n"                                             \
            "stp x2, x3, [sp, #-16]!\n"                                             \
            "stp x4, x5, [sp, #-16]!\n"                                             \
            "stp x6, x7, [sp, #-16]!\n"                                             \
            "str x30, [sp, #-16]!\n"                                                \
            "mov x3, x30\n"                                                         \
            "ldr x2, [sp, #48]\n"                                                   \
            "ldr x1, [sp, #64]\n"                                                   \
            "mov w0, #" #idx "\n"                                                   \
            "bl _au_cmd_note\n"                                                     \
            "ldr x30, [sp], #16\n"                                                  \
            "ldp x6, x7, [sp], #16\n"                                               \
            "ldp x4, x5, [sp], #16\n"                                               \
            "ldp x2, x3, [sp], #16\n"                                               \
            "ldp x0, x1, [sp], #16\n"                                               \
            ".word " #w0 "\n" ".word " #w1 "\n" ".word " #w2 "\n"                   \
            "adrp x16, _" #name "_cont@PAGE\n"                                       \
            "ldr x16, [x16, _" #name "_cont@PAGEOFF]\n"                              \
            "br x16\n");                                                             \
    }
__attribute__((used)) static void *act_stub_cont;
__attribute__((naked)) static void act_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "stp x4, x5, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "str q0, [sp, #-16]!\n"                 /* the action call carries a float in s0 */
        "mov x2, x30\n"                          /* caller LR */
        "fmov w3, s0\n"
        "bl _au_act_note\n"
        "ldr q0, [sp], #16\n"
        "ldr x30, [sp], #16\n"
        "ldp x4, x5, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        ".word 0xd10183ff\n" ".word 0xfd000be8\n" ".word 0xf9000ff9\n"
        "adrp x16, _act_stub_cont@PAGE\n"
        "ldr x16, [x16, _act_stub_cont@PAGEOFF]\n"
        "br x16\n");
}
void au_act_note(uint64_t a0, uint64_t a1, uint64_t caller, uint32_t s0bits);
void au_act_note(uint64_t a0, uint64_t a1, uint64_t caller, uint32_t s0bits) {
    static int n; n++;
    unsigned gate = 0xff;
    if (a0) memcpy(&gate, (void *)(uintptr_t)(a0 + 8), 1);
    if (n <= 40)
        au_log_line("[act] action #%d ctx=%#llx gate8=%u arg1=%#llx s0=%#x caller main+0x%llx\n",
                    n, (unsigned long long)a0, gate, (unsigned long long)a1, s0bits,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
__attribute__((used)) static void *exec_stub_cont;
__attribute__((naked)) static void exec_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "stp x4, x5, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "str q0, [sp, #-16]!\n"
        "mov x2, x30\n"
        "fmov w3, s0\n"
        "bl _au_exec_note\n"
        "ldr q0, [sp], #16\n"
        "ldr x30, [sp], #16\n"
        "ldp x4, x5, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        ".word 0xd101c3ff\n" ".word 0x6d0223e9\n" ".word 0xa9035ff8\n"
        "adrp x16, _exec_stub_cont@PAGE\n"
        "ldr x16, [x16, _exec_stub_cont@PAGEOFF]\n"
        "br x16\n");
}
void au_exec_note(uint64_t a0, uint64_t a1, uint64_t caller, uint32_t s0bits);
void au_exec_note(uint64_t a0, uint64_t a1, uint64_t caller, uint32_t s0bits) {
    static int n; n++;
    if (n <= 30)
        au_log_line("[exec] event #%d node=%#llx actions=%#llx s0=%#x caller main+0x%llx\n",
                    n, (unsigned long long)a0, (unsigned long long)a1, s0bits,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
__attribute__((used)) static void *lookup_stub_cont;
__attribute__((naked)) static void lookup_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "mov x2, x30\n"
        "bl _au_lookup_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        ".word 0xf81c0ff7\n" ".word 0xa90157f6\n" ".word 0xa9024ff4\n"
        "adrp x16, _lookup_stub_cont@PAGE\n"
        "ldr x16, [x16, _lookup_stub_cont@PAGEOFF]\n"
        "br x16\n");
}
void au_lookup_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_lookup_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++;
    if (n <= 20)
        au_log_line("[actlookup] #%d table=%#llx id=%#llx caller main+0x%llx\n", n,
                    (unsigned long long)a0, (unsigned long long)a1,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
__attribute__((used)) static void *play_stub_cont;
__attribute__((naked)) static void play_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "str q0, [sp, #-16]!\n"
        "str q1, [sp, #-16]!\n"
        "mov x2, x30\n"
        "bl _au_play_note\n"
        "ldr q1, [sp], #16\n"
        "ldr q0, [sp], #16\n"
        "ldr x30, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        ".word 0xd10203ff\n" ".word 0x6d0323e9\n" ".word 0xa9045ff8\n"
        "adrp x16, _play_stub_cont@PAGE\n"
        "ldr x16, [x16, _play_stub_cont@PAGEOFF]\n"
        "br x16\n");
}
void au_play_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_play_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++;
    if (n <= 20)
        au_log_line("[actplay] #%d self=%#llx arg=%#llx caller main+0x%llx\n", n,
                    (unsigned long long)a0, (unsigned long long)a1,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
#define AU_SIMPLE_PROBE(sym, words, tag, note_fn) \
    __attribute__((used)) static void *sym##_stub_cont; \
    __attribute__((naked)) static void sym(void) { \
        __asm__ volatile( \
            "stp x0, x1, [sp, #-16]!\n" "stp x2, x3, [sp, #-16]!\n" "stp x4, x5, [sp, #-16]!\n" "stp x6, x7, [sp, #-16]!\n" \
            "str x30, [sp, #-16]!\n" "mov x2, x30\n" \
            "bl " #note_fn "\n" \
            "ldr x30, [sp], #16\n" "ldp x6, x7, [sp], #16\n" "ldp x4, x5, [sp], #16\n" "ldp x2, x3, [sp], #16\n" "ldp x0, x1, [sp], #16\n" \
            words \
            "adrp x16, _" #sym "_stub_cont@PAGE\n" \
            "ldr x16, [x16, _" #sym "_stub_cont@PAGEOFF]\n" \
            "br x16\n"); \
    }
AU_SIMPLE_PROBE(actgo, ".word 0x6dbb23e9\n.word 0xa9015ff8\n.word 0xa90257f6\n", "actgo", _au_actgo_note)
AU_SIMPLE_PROBE(actskip, ".word 0xb9000be8\n.word 0xf9400428\n.word 0x9100218b\n", "actskip", _au_actskip_note)
static void (*const au_actskip_unused_ref)(void) __attribute__((unused)) = actskip;
void au_actgo_note(uint64_t a0, uint64_t a1, uint64_t a2);
void au_actgo_note(uint64_t a0, uint64_t a1, uint64_t a2) {
    static int n; n++;
    if (n <= 20) au_log_line("[actgo] #%d a0=%#llx a1=%#llx caller main+0x%llx\n", n,
        (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)au_modoff((void *)(uintptr_t)a2));
}
void au_actskip_note(uint64_t a0, uint64_t a1, uint64_t a2);
void au_actskip_note(uint64_t a0, uint64_t a1, uint64_t a2) {
    static int n; n++;
    if (n <= 20) au_log_line("[actskip] #%d a0=%#llx a1=%#llx caller main+0x%llx\n", n,
        (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)au_modoff((void *)(uintptr_t)a2));
}
#define AU_FLOAT_PROBE(sym, words, note_fn) \
    __attribute__((used)) static void *sym##_stub_cont; \
    __attribute__((naked)) static void sym(void) { \
        __asm__ volatile( \
            "stp x0, x1, [sp, #-16]!\n" "stp x2, x3, [sp, #-16]!\n" "stp x4, x5, [sp, #-16]!\n" "stp x6, x7, [sp, #-16]!\n" \
            "str x30, [sp, #-16]!\n" "str q0, [sp, #-16]!\n" "str q1, [sp, #-16]!\n" \
            "str q2, [sp, #-16]!\n" "str q3, [sp, #-16]!\n" \
            "mov x2, x30\n" "bl " #note_fn "\n" \
            "ldr q3, [sp], #16\n" "ldr q2, [sp], #16\n" "ldr q1, [sp], #16\n" "ldr q0, [sp], #16\n" \
            "ldr x30, [sp], #16\n" \
            "ldp x6, x7, [sp], #16\n" "ldp x4, x5, [sp], #16\n" "ldp x2, x3, [sp], #16\n" "ldp x0, x1, [sp], #16\n" \
            words \
            "adrp x16, _" #sym "_stub_cont@PAGE\n" \
            "ldr x16, [x16, _" #sym "_stub_cont@PAGEOFF]\n" \
            "br x16\n"); \
    }
AU_FLOAT_PROBE(post1, ".word 0xf81b0ff9\n.word 0xa9015ff8\n.word 0xa90257f6\n", _au_post1_note)
AU_FLOAT_PROBE(post2, ".word 0xfc1b0fe8\n.word 0xf90007f9\n.word 0xa9015ff8\n", _au_post2_note)
/* The play request's own per-frame tick: (req, frame) -> nonzero means "drop this request".  The state in
   [req+0x3c] tells whether a request ever leaves the queued state, and the two fields at +0x18/+0x1c are the
   playback window it is waiting for. */
AU_FLOAT_PROBE(reqtick, ".word 0xfc1d0fe8\n.word 0xf90007f5\n.word 0xa9014ff4\n", _au_reqtick_note)
static void au_test_play_note(void);
void au_frameprobe_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_frameprobe_note(uint64_t a0, uint64_t a1, uint64_t caller) { (void)a0; (void)a1; (void)caller; au_test_play_note(); }

void au_akinit_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_akinit_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 5 || (n % 20) == 0)
        au_log_line("[aklife] akinit #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(akinit, ".word 0xd10183ff\n.word 0xa90167fa\n.word 0xa9025ff8\n", _au_akinit_note)
void au_akterm_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_akterm_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 5 || (n % 20) == 0)
        au_log_line("[aklife] akterm #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(akterm, ".word 0xa9ba6ffc\n.word 0xa90167fa\n.word 0xa9025ff8\n", _au_akterm_note)

void au_mapA_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_mapA_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[mapuse] mapA #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(mapA, ".word 0xd10143ff\n.word 0xa9015ff8\n.word 0xa90257f6\n", _au_mapA_note)
void au_mapB_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_mapB_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[mapuse] mapB #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(mapB, ".word 0xd10203ff\n.word 0xa90367fa\n.word 0xa9045ff8\n", _au_mapB_note)
void au_mapC_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_mapC_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[mapuse] mapC #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(mapC, ".word 0xf81c0ff9\n.word 0xa9015ff8\n.word 0xa90257f6\n", _au_mapC_note)
void au_mapD_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_mapD_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[mapuse] mapD #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(mapD, ".word 0xa9ba6ffc\n.word 0xa90167fa\n.word 0xa9025ff8\n", _au_mapD_note)
void au_mapE_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_mapE_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[mapuse] mapE #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(mapE, ".word 0x6dbd23e9\n.word 0xa9014ff4\n.word 0xa9027bfd\n", _au_mapE_note)

void au_drainA_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_drainA_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[drain] drainA #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(drainA, ".word 0xa9bc5ff8\n.word 0xa90157f6\n.word 0xa9024ff4\n", _au_drainA_note)
void au_drainB_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_drainB_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[drain] drainB #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(drainB, ".word 0xd10203ff\n.word 0xfd001be8\n.word 0xa9045ff8\n", _au_drainB_note)
void au_drainC_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_drainC_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[drain] drainC #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(drainC, ".word 0xd101c3ff\n.word 0xf90013f9\n.word 0xa9035ff8\n", _au_drainC_note)
void au_drainD_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_drainD_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[drain] drainD #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(drainD, ".word 0xfc1b0fe8\n.word 0xf90007f9\n.word 0xa9015ff8\n", _au_drainD_note)
void au_drainE_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_drainE_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[drain] drainE #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(drainE, ".word 0xa9bc5ff8\n.word 0xa90157f6\n.word 0xa9024ff4\n", _au_drainE_note)
void au_drainF_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_drainF_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[drain] drainF #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(drainF, ".word 0xf81a0ffb\n.word 0xa90167fa\n.word 0xa9025ff8\n", _au_drainF_note)
void au_drainG_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_drainG_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[drain] drainG #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(drainG, ".word 0xd10183ff\n.word 0xa90167fa\n.word 0xa9025ff8\n", _au_drainG_note)

void au_ringA_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_ringA_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[ring] ringA #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(ringA, ".word 0xd10243ff\n.word 0xf9001bfb\n.word 0xa90467fa\n", _au_ringA_note)
void au_ringB_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_ringB_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[ring] ringB #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(ringB, ".word 0xa9be4ff4\n.word 0xa9017bfd\n.word 0x910043fd\n", _au_ringB_note)
void au_ringC_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_ringC_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[ring] ringC #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(ringC, ".word 0xa9be4ff4\n.word 0xa9017bfd\n.word 0x910043fd\n", _au_ringC_note)
void au_ringD_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_ringD_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[ring] ringD #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(ringD, ".word 0xf81d0ff5\n.word 0xa9014ff4\n.word 0xa9027bfd\n", _au_ringD_note)
void au_ringE_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_ringE_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[ring] ringE #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(ringE, ".word 0xf81d0ff5\n.word 0xa9014ff4\n.word 0xa9027bfd\n", _au_ringE_note)
void au_ringF_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_ringF_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[ring] ringF #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(ringF, ".word 0xf81b0ff9\n.word 0xa9015ff8\n.word 0xa90257f6\n", _au_ringF_note)
void au_ringG_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_ringG_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 3 || (n % 500) == 0)
        au_log_line("[ring] ringG #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(ringG, ".word 0xd10143ff\n.word 0xf90013f5\n.word 0xa9034ff4\n", _au_ringG_note)

/* The AK event-manager's own handlers: type 1 = "play on game object", then its two callees.  These are the only
   places left between a posted event and a created voice; counting them tells whether the engine ever enters the
   playback branch for the game object the game actually uses. */
AU_FLOAT_PROBE(evdisp, ".word 0xf940a329\n.word 0xb940392a\n.word 0x34002cea\n", _au_evdisp_note)
AU_FLOAT_PROBE(execon, ".word 0xd101c3ff\n.word 0xa9016ffc\n.word 0xa90267fa\n", _au_execon_note)
AU_FLOAT_PROBE(exeplay, ".word 0xd10283ff\n.word 0xf9002bf9\n.word 0xa9065ff8\n", _au_exeplay_note)
/* The game's sound-source tick and the AK driver object it forwards to. */
AU_FLOAT_PROBE(srcupd, ".word 0xa9be4ff4\n.word 0xa9017bfd\n.word 0x910043fd\n", _au_srcupd_note)
AU_FLOAT_PROBE(drvupd, ".word 0x6dbb23e9\n.word 0xa9015ff8\n.word 0xa90257f6\n", _au_drvupd_note)
void au_post1_note(uint64_t a0, uint64_t a1, uint64_t a2);
void au_post1_note(uint64_t a0, uint64_t a1, uint64_t a2) {
    static int n; n++;
    if (n <= 20) au_log_line("[post1] #%d a0=%#llx a1=%#llx caller main+0x%llx\n", n,
        (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)au_modoff((void *)(uintptr_t)a2));
}
void au_post2_note(uint64_t a0, uint64_t a1, uint64_t a2);
void au_post2_note(uint64_t a0, uint64_t a1, uint64_t a2) {
    static int n; n++;
    if (n <= 20) au_log_line("[post2] #%d a0=%#llx a1=%#llx caller main+0x%llx\n", n,
        (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)au_modoff((void *)(uintptr_t)a2));
}
void au_srcupd_note(uint64_t a0, uint64_t a1, uint64_t a2);
void au_srcupd_note(uint64_t a0, uint64_t a1, uint64_t a2) {
    static int n; (void)a1; n++;
    if (n <= 8 || n % 300 == 0) {
        uint64_t drv = 0, dvt = 0; uint8_t fl = 0xff;
        if (AU_PTR_OK(a0)) {
            memcpy(&drv, (void *)(uintptr_t)(a0 + 0x18), 8);
            if (AU_PTR_OK(drv)) { memcpy(&dvt, (void *)(uintptr_t)drv, 8); memcpy(&fl, (void *)(uintptr_t)(drv + 8), 1); }
        }
        au_log_line("[srcupd] #%d src=%#llx drv=%#llx drvvt=main+0x%llx drvfl=%u caller main+0x%llx\n", n,
            (unsigned long long)a0, (unsigned long long)drv, (unsigned long long)au_modoff((void *)(uintptr_t)dvt), fl,
            (unsigned long long)au_modoff((void *)(uintptr_t)a2));
    }
}

/* PLA_TEST_PLAY=<frame>:<event>[,<event>...]  (diagnostic): post a real Wwise play event from the guest main thread
   once the app has run <frame> frames.  PostEvent is designed to be called from the game thread, so this is the
   supported direction (unlike calling into the engine from the host).  Purpose: make a play event happen on demand so
   the request/source/driver/voice probes can be exercised without a human playing. */
static char g_test_play_names[128];
static void au_test_play_note(void) {
    static long n;
    static int before, after;
    if (!getenv("PLA_TEST_PLAY")) return;
    n++;
    if (!after && n == before + 1) {
        if (!before) {
            const char *spec = getenv("PLA_TEST_PLAY");
            long at = strtol(spec, NULL, 10);
            const char *names = strchr(spec, ':'); names = names ? names + 1 : "PLAY_UI_MESSAGE";
            before = (int)(at > 0 ? at : 2000);
            au_log_line("[testplay] armed: post \"%s\" at frame %d\n", names, before);
            snprintf(g_test_play_names, sizeof g_test_play_names, "%s", names);
            return;
        }
        if (n < before) return;
        after = 1;
        const char *nm = g_test_play_names;
        char one[64];
        while (nm && *nm) {
            const char *comma = strchr(nm, ','); size_t len = comma ? (size_t)(comma - nm) : strlen(nm);
            if (len >= sizeof one) len = sizeof one - 1;
            memcpy(one, nm, len); one[len] = 0;
            int (*post)(const char *, uint64_t, uint32_t) =
                (int (*)(const char *, uint64_t, uint32_t))(void *)(runtime_symbol("nnMain") ? ((uintptr_t)runtime_symbol("nnMain") - 0x2779f70ULL + 0x2d77240ULL) : 0);
            int r = post ? post(one, 1, 1) : -1;
            au_log_line("[testplay] PostEvent(\"%s\", go=1) -> %d (n=%ld)\n", one, r, n);
            nm = comma ? comma + 1 : NULL;
        }
    }
}

void au_drvupd_note(uint64_t a0, uint64_t a1, uint64_t a2);
void au_drvupd_note(uint64_t a0, uint64_t a1, uint64_t a2) {
    static int n; n++;
    if (n <= 8 || n % 300 == 0) {
        uint64_t sub = 0, svt = 0; uint32_t cnt = 0;
        if (AU_PTR_OK(a0)) {
            memcpy(&sub, (void *)(uintptr_t)(a0 + 0x10), 8);
            if (AU_PTR_OK(sub)) { memcpy(&svt, (void *)(uintptr_t)sub, 8); memcpy(&cnt, (void *)(uintptr_t)(sub + 0x18), 4); }
        }
        uint64_t d0 = 0, rd = a0 ? a0 - 0x18 : 0, rd0 = 0, rd10 = 0, rsub = 0, rsvt = 0; uint32_t rcnt = 0;
        if (AU_PTR_OK(a0)) memcpy(&d0, (void *)(uintptr_t)a0, 8);
        if (AU_PTR_OK(rd)) {
            memcpy(&rd0, (void *)(uintptr_t)rd, 8);
            memcpy(&rd10, (void *)(uintptr_t)(rd + 0x10), 8);
            if (AU_PTR_OK(rd10)) {
                memcpy(&rsub, (void *)(uintptr_t)(rd10 + 0x10), 8);
                if (AU_PTR_OK(rsub)) { memcpy(&rsvt, (void *)(uintptr_t)rsub, 8); memcpy(&rcnt, (void *)(uintptr_t)(rsub + 0x18), 4); }
            }
        }
        au_log_line("[drvupd] #%d drv=%#llx this_vt=main+0x%llx base_vt=main+0x%llx base+0x10=%#llx rsub=%#llx rsubvt=main+0x%llx rsub_n=%u w1=%llu caller main+0x%llx\n", n,
            (unsigned long long)a0, (unsigned long long)au_modoff((void *)(uintptr_t)d0),
            (unsigned long long)au_modoff((void *)(uintptr_t)rd0), (unsigned long long)rd10,
            (unsigned long long)rsub, (unsigned long long)au_modoff((void *)(uintptr_t)rsvt), rcnt,
            (unsigned long long)a1, (unsigned long long)au_modoff((void *)(uintptr_t)a2));
    }
}

void au_evdisp_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_evdisp_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a1;
    uint8_t *mb = (uint8_t *)g_site_modbase; unsigned nb = 0; uint64_t go = 0; int found = 0;
    if (AU_PTR_OK((void *)(uintptr_t)a0)) memcpy(&go, (void *)(uintptr_t)(a0 + 4), 8);   /* message layout: gameObjectID at +4 (see 0x2d7ec5c) */
    if (mb) { uint64_t tbl = 0; memcpy(&tbl, mb + 0x430a140, 8);
              if (AU_PTR_OK(tbl)) { memcpy(&nb, (void *)(uintptr_t)(tbl + 0x38), 4);
                 uint64_t arr = 0; if (nb && nb < 4096) memcpy(&arr, (void *)(uintptr_t)(tbl + 0x30), 8);
                 if (AU_PTR_OK(arr)) { uint64_t h = 0; memcpy(&h, (void *)(uintptr_t)(arr + (go % nb) * 8), 8);
                    for (int d = 0; d < 64 && AU_PTR_OK(h); d++) {
                        uint64_t id = 0, nx = 0;
                        memcpy(&id, (void *)(uintptr_t)(h + 8), 8); memcpy(&nx, (void *)(uintptr_t)h, 8);
                        if (id == go) found = 1; h = nx; } } } }
    if (n <= 20 || (n % 100) == 0)
        au_log_line("[evdisp] #%d msg=%#llx go=%#llx buckets=%u found=%u caller main+0x%llx\n",
                    n, (unsigned long long)a0, (unsigned long long)go, nb, (unsigned)found,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
void au_execon_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_execon_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++;
    if (n <= 20 || (n % 100) == 0)
        au_log_line("[execon] #%d a0=%#llx a1=%#llx caller main+0x%llx\n", n,
                    (unsigned long long)a0, (unsigned long long)a1,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
void au_exeplay_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_exeplay_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++;
    if (n <= 20 || (n % 100) == 0)
        au_log_line("[exeplay] #%d a0=%#llx a1=%#llx caller main+0x%llx\n", n,
                    (unsigned long long)a0, (unsigned long long)a1,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}

void au_reqtick_note(uint64_t a0, uint64_t a1, uint64_t a2);
void au_reqtick_note(uint64_t a0, uint64_t a1, uint64_t a2) {
    static int n; n++;
    if (n <= 24 || n % 200 == 0) {
        uint32_t state = 0, lo = 0, hi = 0, nsrc = 0; uint8_t fl = 0xff; uint64_t srcarr = 0, src0 = 0, srcvt = 0, drv = 0, drvvt = 0; uint32_t sid = 0; uint8_t drvfl = 0xff;
        if (AU_PTR_OK(a0)) {
            memcpy(&state, (void *)(uintptr_t)(a0 + 0x3c), 4);
            memcpy(&lo, (void *)(uintptr_t)(a0 + 0x18), 4);
            memcpy(&hi, (void *)(uintptr_t)(a0 + 0x1c), 4);
            memcpy(&fl, (void *)(uintptr_t)(a0 + 0x40), 1);
            memcpy(&nsrc, (void *)(uintptr_t)(a0 + 0x30), 4);
            memcpy(&srcarr, (void *)(uintptr_t)(a0 + 0x28), 8);
            if (nsrc && AU_PTR_OK(srcarr)) {
                memcpy(&src0, (void *)(uintptr_t)srcarr, 8);
                if (AU_PTR_OK(src0)) {
                    memcpy(&srcvt, (void *)(uintptr_t)src0, 8);
                    memcpy(&drv, (void *)(uintptr_t)(src0 + 0x18), 8);
                    memcpy(&sid, (void *)(uintptr_t)(src0 + 0x38), 4);
                    if (AU_PTR_OK(drv)) {
                        memcpy(&drvvt, (void *)(uintptr_t)drv, 8);
                        memcpy(&drvfl, (void *)(uintptr_t)(drv + 8), 1);
                    }
                }
            }
        }
        au_log_line("[reqtick] #%d req=%#llx frame=%llu state=%u nsrc=%u sid=%u srcvt=main+0x%llx drv=%#llx drvvt=main+0x%llx drvfl=%u caller main+0x%llx\n", n,
            (unsigned long long)a0, (unsigned long long)a1, state, nsrc, sid,
            (unsigned long long)au_modoff((void *)(uintptr_t)srcvt), (unsigned long long)drv,
            (unsigned long long)au_modoff((void *)(uintptr_t)drvvt), drvfl,
            (unsigned long long)au_modoff((void *)(uintptr_t)a2));
    }
}
AU_CMD_STUB(cmd_evpost_stub, 0, 0xf81b0ff9, 0xa9015ff8, 0xa90257f6)
AU_CMD_STUB(cmd_alloc_stub, 1, 0xd101c3ff, 0xa9016ffc, 0xa90267fa)
AU_CMD_STUB(cmd_nocmd_stub, 2, 0xd101c3ff, 0xf9000bfb, 0xa90267fa)
AU_CMD_STUB(cmd_ring_stub, 3, 0xd10443ff, 0xa90b6ffc, 0xa90c67fa)
AU_CMD_STUB(cmd_msgdone_stub, 4, 0x2a1f03e8, 0x79400349, 0x11000d2a)
AU_CMD_STUB(cmd_goevent_stub, 5, 0xd2c00088, 0xf90043e8, 0x390223ff)
AU_CMD_STUB(cmd_gofound_stub, 6, 0xb940510a, 0x1100054a, 0xb900510a)
AU_CMD_STUB(cmd_action_stub, 7, 0xd10183ff, 0xfd000be8, 0xf9000ff9)
AU_CMD_STUB(cmd_reggo_stub, 10, 0xa9be4ff4, 0xa9017bfd, 0x910043fd)
AU_CMD_STUB(cmd_unreggo_stub, 11, 0xa9be4ff4, 0xa9017bfd, 0x910043fd)
__attribute__((used)) static void *req_m20_cont, *req_m40_cont, *req_m60_cont, *req_m80_cont, *req_ma0_cont;
AU_CMD_STUB(req_m20, 45, 0xf81d0ff5, 0xa9014ff4, 0xa9027bfd)
AU_CMD_STUB(req_m40, 46, 0xf81d0ff5, 0xa9014ff4, 0xa9027bfd)
AU_CMD_STUB(req_m60, 47, 0xf81d0ff5, 0xa9014ff4, 0xa9027bfd)
AU_CMD_STUB(req_m80, 48, 0xf81d0ff5, 0xa9014ff4, 0xa9027bfd)
AU_CMD_STUB(req_ma0, 49, 0xf81d0ff5, 0xa9014ff4, 0xa9027bfd)
AU_CMD_STUB(cmd_factory_stub, 12, 0xf81d0ff5, 0xa9014ff4, 0xa9027bfd)
AU_CMD_STUB(cmd_sceneinit_stub, 13, 0xd100c3ff, 0xf9000bf3, 0xa9027bfd)
AU_CMD_STUB(chain_sceneupd_stub, 20, 0xd10583ff, 0xa9106ffc, 0xa91167fa)
AU_CMD_STUB(chain_ch1_stub, 21, 0xa9bd57f6, 0xa9014ff4, 0xa9027bfd)
AU_CMD_STUB(chain_ch2_stub, 22, 0xd10103ff, 0xf9000bf5, 0xa9024ff4)
AU_CMD_STUB(chain_ch3_stub, 23, 0xf81e0ff3, 0xa9017bfd, 0x910043fd)
AU_CMD_STUB(chain_factory2_stub, 24, 0xa9bd57f6, 0xa9014ff4, 0xa9027bfd)
AU_CMD_STUB(chain_scinit_stub, 25, 0xd103c3ff, 0xa9096ffc, 0xa90a67fa)
AU_CMD_STUB(chain_famctor_stub, 26, 0xd100c3ff, 0xf9000bf3, 0xa9027bfd)
AU_CMD_STUB(chain_famsib_stub, 27, 0xd10203ff, 0xa9026ffc, 0xa90367fa)
AU_CMD_STUB(chain_famupd_stub, 28, 0xf81e0ff3, 0xa9017bfd, 0x910043fd)
AU_CMD_STUB(chain_sndtop_stub, 29, 0xa9ba6ffc, 0xa90167fa, 0xa9025ff8)
AU_CMD_STUB(chain_sndbig_stub, 30, 0xd10603ff, 0xa9126ffc, 0xa91367fa)
AU_CMD_STUB(chain_ready_stub, 31, 0xa9be4ff4, 0xa9017bfd, 0x910043fd)
AU_CMD_STUB(chain_st1_stub, 32, 0xf81a0ffb, 0xa90167fa, 0xa9025ff8)
AU_CMD_STUB(chain_st2_stub, 33, 0xf9402c09, 0xb9400128, 0x7100111f)
AU_CMD_STUB(chain_st3_stub, 34, 0xd10143ff, 0xf90013f5, 0xa9034ff4)
AU_CMD_STUB(chain_postev_stub, 35, 0xa9ba6ffc, 0xa90167fa, 0xa9025ff8)
AU_CMD_STUB(chain_usercmd_stub, 41, 0xf81b0ff9, 0xa9015ff8, 0xa90257f6)
AU_CMD_STUB(chain_reggo_stub, 42, 0xa9be4ff4, 0xa9017bfd, 0x910043fd)
AU_CMD_STUB(mixdev_stub, 50, 0xd10143ff, 0xa9015ff8, 0xa90257f6)
AU_CMD_STUB(voicecreate_stub, 51, 0xd101c3ff, 0xa90267fa, 0xa9035ff8)
AU_CMD_STUB(vcbuild_stub, 52, 0xf81b0ff9, 0xa9015ff8, 0xa90257f6)
AU_CMD_STUB(vccaller1_stub, 53, 0xd10143ff, 0xa9015ff8, 0xa90257f6)
AU_CMD_STUB(vccaller2_stub, 54, 0xd101c3ff, 0xf90013fb, 0xa90267fa)

/* PLA_AUDIO_CHAIN_PROBE=1 (diagnostic): count the six entry points of the game-side sound-object construction
   chain so we can tell exactly which link is missing.  Every prologue below is three non-PC-relative words, so the
   same replay-three-words stub as the command probe works.  Called from runtime_audio_bank_bootstrap_install()
   while the text is still writable (a hook installed later faults: esr=0x9200004f). */
/* PLA_REQ_PROBE=1 (diagnostic): the play-request object implements four interfaces; five real methods all look up
   an engine singleton and forward to [singleton->vtable + 0xc0].  Counting them and printing the forward target
   tells us whether voice creation is even reached. */
void runtime_audio_req_probe_install(void);
void runtime_audio_req_probe_install(void) {
    if (!getenv("PLA_REQ_PROBE")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    if (!g_site_modbase) g_site_modbase = base;
    struct { uint32_t off; void (*stub)(void); void **cont; const char *tag; } sites[] = {
        {0x2de3ecc, req_m20, &req_m20_cont, "req.m20"},
        {0x2de401c, req_m40, &req_m40_cont, "req.m40"},
        {0x2de5e88, req_m60, &req_m60_cont, "req.m60"},
        {0x2de5fd8, req_m80, &req_m80_cont, "req.m80"},
        {0x2de64f8, req_ma0, &req_ma0_cont, "req.ma0"},
    };
    int ok = 0;
    for (unsigned i = 0; i < sizeof sites / sizeof sites[0]; i++) {
        uint8_t *fn = base + sites[i].off;
        uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
        if (ex[0] != 0xf81d0ff5u || ex[1] != 0xa9014ff4u || ex[2] != 0xa9027bfdu) {
            fprintf(stderr, "[req] %s prologue differs (%#x %#x %#x), skipped\n", sites[i].tag, ex[0], ex[1], ex[2]);
            continue;
        }
        *sites[i].cont = fn + 12;
        uint32_t pt[3] = {0, 0, 0};
        emit_adrp_br(pt, (uintptr_t)fn, (void *)sites[i].stub);
        memcpy(fn, pt, sizeof pt);
        sys_icache_invalidate(fn, 16);
        ok++;
    }
    fprintf(stderr, "[req] %d request-interface sites hooked\n", ok);
}

/* Minimal AK-path probe set (PLA_AUDIO_SNDCHAIN_PROBE=1 PLA_AUDIO_SNDCHAIN_SITES=4): event dispatch, action
   execution, CActionPlay and the voice allocator.  Deliberately tiny, because the 15-site set perturbs the AK
   timing enough that the registration command never reaches the engine. */


void au_srcvt_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_srcvt_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 6 || (n % 200) == 0)
        au_log_line("[akpath] srcvt #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(srcvt, ".word 0xa9be4ff4\n.word 0xa9017bfd\n.word 0x910043fd\n", _au_srcvt_note)

void au_vcbuild_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_vcbuild_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    au_log_line("[akpath] vcbuild #%d caller main+0x%llx\n", n,
                (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(vcbuild, ".word 0xf81b0ff9\n.word 0xa9015ff8\n.word 0xa90257f6\n", _au_vcbuild_note)
void au_vplstart_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_vplstart_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    au_log_line("[akpath] vplstart #%d caller main+0x%llx\n", n,
                (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(vplstart, ".word 0xd10143ff\n.word 0xa9015ff8\n.word 0xa90257f6\n", _au_vplstart_note)

void au_actplay2_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_actplay2_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    if (n <= 5 || (n % 20) == 0)
        au_log_line("[akpath] actplay #%d caller main+0x%llx\n", n,
                    (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(actplay2, ".word 0xd10203ff\n.word 0x6d0323e9\n.word 0xa9045ff8\n", _au_actplay2_note)
void au_voicecr_note(uint64_t a0, uint64_t a1, uint64_t caller);
void au_voicecr_note(uint64_t a0, uint64_t a1, uint64_t caller) {
    static int n; n++; (void)a0; (void)a1;
    au_log_line("[akpath] VOICE CREATE #%d caller main+0x%llx\n", n,
                (unsigned long long)au_modoff((void *)(uintptr_t)caller));
}
AU_FLOAT_PROBE(voicecr, ".word 0xd101c3ff\n.word 0xa90267fa\n.word 0xa9035ff8\n", _au_voicecr_note)

void runtime_audio_sndchain_probe_install(void);
void runtime_audio_sndchain_probe_install(void) {
    if (!getenv("PLA_AUDIO_SNDCHAIN_PROBE")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    if (!g_site_modbase) g_site_modbase = base;
    struct { uint32_t off; uint32_t w[3]; void (*stub)(void); void **cont; const char *tag; } sites[] = {
        
        
        {0x2de13f4, {0xd10203ffu, 0x6d0323e9u, 0xa9045ff8u}, actplay2, &actplay2_stub_cont, "actplay"},
        {0x2e150f4, {0xd101c3ffu, 0xa90267fau, 0xa9035ff8u}, voicecr, &voicecr_stub_cont, "voicecreate"},
        {0x2e14e14, {0xf81b0ff9u, 0xa9015ff8u, 0xa90257f6u}, vcbuild, &vcbuild_stub_cont, "vcbuild"},
        {0x2e147fc, {0xd10143ffu, 0xa9015ff8u, 0xa90257f6u}, vplstart, &vplstart_stub_cont, "vplstart"},
        {0x2e0e5cc, {0xfc1d0fe8u, 0xf90007f5u, 0xa9014ff4u}, reqtick, &reqtick_stub_cont, "reqtick"},
        {0x2e05638, {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, srcvt, &srcvt_stub_cont, "srcvt"},
        {0x2dcfcbc, {0x6dbb23e9u, 0xa9015ff8u, 0xa90257f6u}, drvupd, &drvupd_stub_cont, "drvupd"},
        {0x1844770, {0xd10583ffu, 0xa9106ffcu, 0xa91167fau}, chain_sceneupd_stub, &chain_sceneupd_stub_cont, "sceneupd"},
        {0x1dd7410, {0xa9bd57f6u, 0xa9014ff4u, 0xa9027bfdu}, chain_ch1_stub, &chain_ch1_stub_cont, "ch1"},
        {0x1dd755c, {0xd10103ffu, 0xf9000bf5u, 0xa9024ff4u}, chain_ch2_stub, &chain_ch2_stub_cont, "ch2"},
        {0x1dd971c, {0xf81e0ff3u, 0xa9017bfdu, 0x910043fdu}, chain_ch3_stub, &chain_ch3_stub_cont, "ch3"},
        {0x1dd9874, {0xa9bd57f6u, 0xa9014ff4u, 0xa9027bfdu}, chain_factory2_stub, &chain_factory2_stub_cont, "factory"},
        {0x1dd7c68, {0xd103c3ffu, 0xa9096ffcu, 0xa90a67fau}, chain_scinit_stub, &chain_scinit_stub_cont, "scinit"},
        {0x1844514, {0xd100c3ffu, 0xf9000bf3u, 0xa9027bfdu}, chain_famctor_stub, &chain_famctor_stub_cont, "famctor"},   /* base ctor installing vtable blob+0x98 */
        {0x1843ebc, {0xd10203ffu, 0xa9026ffcu, 0xa90367fau}, chain_famsib_stub, &chain_famsib_stub_cont, "famsib"},     /* sibling base ctor (blob+0x1a0) */
        {0x1844700, {0xf81e0ff3u, 0xa9017bfdu, 0x910043fdu}, chain_famupd_stub, &chain_famupd_stub_cont, "famupd"},     /* slot 0x4130100, same vtable */
        {0x1877a4c, {0xa9ba6ffcu, 0xa90167fau, 0xa9025ff8u}, chain_sndtop_stub, &chain_sndtop_stub_cont, "sndtop"},    /* registered slot 0x4132770, no BL callers */
        {0x18b5b5c, {0xd10603ffu, 0xa9126ffcu, 0xa91367fau}, chain_sndbig_stub, &chain_sndbig_stub_cont, "sndbig"},    /* 24KB, builds 8 sub-objects */
        {0x8470dc, {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, chain_ready_stub, &chain_ready_stub_cont, "ready"},   /* writes app_state=6 */
        {0x2844350, {0xf81a0ffbu, 0xa90167fau, 0xa9025ff8u}, chain_st1_stub, &chain_st1_stub_cont, "appst1"},    /* writes app_state 1|4 */
        {0x2848360, {0xf9402c09u, 0xb9400128u, 0x7100111fu}, chain_st2_stub, &chain_st2_stub_cont, "appst2"},    /* writes app_state=4 */
        {0x1b01684, {0xd10143ffu, 0xf90013f5u, 0xa9034ff4u}, chain_st3_stub, &chain_st3_stub_cont, "appst3"},   /* writes app_state */
        {0x2d77020, {0xa9ba6ffcu, 0xa90167fau, 0xa9025ff8u}, chain_postev_stub, &chain_postev_stub_cont, "postevent"},
        {0x2d76f2c, {0xf81b0ff9u, 0xa9015ff8u, 0xa90257f6u}, chain_usercmd_stub, &chain_usercmd_stub_cont, "usercmd"},
        {0x28372cc, {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, chain_reggo_stub, &chain_reggo_stub_cont, "reggo"},
        {0x2de0d64, {0xd10183ffu, 0xfd000be8u, 0xf9000ff9u}, act_stub, &act_stub_cont, "action"},        /* action executor (gate [ctx+8]) */
        {0x2de0fe0, {0xd101c3ffu, 0x6d0223e9u, 0xa9035ff8u}, exec_stub, &exec_stub_cont, "actexec"},       /* event action list executor */
        {0x2dddc48, {0xf81c0ff7u, 0xa90157f6u, 0xa9024ff4u}, lookup_stub, &lookup_stub_cont, "actlookup"},
        {0x2de13f4, {0xd10203ffu, 0x6d0323e9u, 0xa9045ff8u}, play_stub, &play_stub_cont, "actplay"},
        {0x2de1bec, {0x6dbb23e9u, 0xa9015ff8u, 0xa90257f6u}, actgo, &actgo_stub_cont, "actgo"},
        {0x2e0ed58, {0xf81b0ff9u, 0xa9015ff8u, 0xa90257f6u}, post1, &post1_stub_cont, "post1"},
        {0x2e0f0a8, {0xfc1b0fe8u, 0xf90007f9u, 0xa9015ff8u}, post2, &post2_stub_cont, "post2"},
        {0x2e0e5cc, {0xfc1d0fe8u, 0xf90007f5u, 0xa9014ff4u}, reqtick, &reqtick_stub_cont, "reqtick"},
        {0x2e05638, {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, srcupd, &srcupd_stub_cont, "srcupd"},   /* game sound-source tick: forwards to the AK driver */
        {0x2dcfcbc, {0x6dbb23e9u, 0xa9015ff8u, 0xa90257f6u}, drvupd, &drvupd_stub_cont, "drvupd"},   /* AK driver update: sub-object list length */   /* per-frame request tick: does a request ever leave state 1? */
        {0x2d74ee0, {0xd10183ffu, 0xa90167fau, 0xa9025ff8u}, akinit, &akinit_stub_cont, "akinit"},
        {0x2d758c0, {0xa9ba6ffcu, 0xa90167fau, 0xa9025ff8u}, akterm, &akterm_stub_cont, "akterm"},
        {0x2e16c00, {0xd10143ffu, 0xa9015ff8u, 0xa90257f6u}, mapA, &mapA_stub_cont, "mapA"},
        {0x2e18c50, {0xd10203ffu, 0xa90367fau, 0xa9045ff8u}, mapB, &mapB_stub_cont, "mapB"},
        {0x2e1c9a0, {0xf81c0ff9u, 0xa9015ff8u, 0xa90257f6u}, mapC, &mapC_stub_cont, "mapC"},
        {0x2e1e720, {0xa9ba6ffcu, 0xa90167fau, 0xa9025ff8u}, mapD, &mapD_stub_cont, "mapD"},
        {0x2e30254, {0x6dbd23e9u, 0xa9014ff4u, 0xa9027bfdu}, mapE, &mapE_stub_cont, "mapE"},
        {0x2db69d8, {0xa9bc5ff8u, 0xa90157f6u, 0xa9024ff4u}, drainA, &drainA_stub_cont, "drainA"},
        {0x2dd184c, {0xd10203ffu, 0xfd001be8u, 0xa9045ff8u}, drainB, &drainB_stub_cont, "drainB"},
        {0x2daafd0, {0xd101c3ffu, 0xf90013f9u, 0xa9035ff8u}, drainC, &drainC_stub_cont, "drainC"},
        {0x2dabb70, {0xfc1b0fe8u, 0xf90007f9u, 0xa9015ff8u}, drainD, &drainD_stub_cont, "drainD"},
        {0x2dabf78, {0xa9bc5ff8u, 0xa90157f6u, 0xa9024ff4u}, drainE, &drainE_stub_cont, "drainE"},
        {0x2dd41a8, {0xf81a0ffbu, 0xa90167fau, 0xa9025ff8u}, drainF, &drainF_stub_cont, "drainF"},
        {0x2d8709c, {0xd10183ffu, 0xa90167fau, 0xa9025ff8u}, drainG, &drainG_stub_cont, "drainG"},
        {0x2e28618, {0xd10243ffu, 0xf9001bfbu, 0xa90467fau}, ringA, &ringA_stub_cont, "ringA"},
        {0x2e28c70, {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, ringB, &ringB_stub_cont, "ringB"},
        {0x2e28d14, {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, ringC, &ringC_stub_cont, "ringC"},
        {0x2e28dc0, {0xf81d0ff5u, 0xa9014ff4u, 0xa9027bfdu}, ringD, &ringD_stub_cont, "ringD"},
        {0x2e28f88, {0xf81d0ff5u, 0xa9014ff4u, 0xa9027bfdu}, ringE, &ringE_stub_cont, "ringE"},
        {0x2e17a04, {0xf81b0ff9u, 0xa9015ff8u, 0xa90257f6u}, ringF, &ringF_stub_cont, "ringF"},
        {0x2e29984, {0xd10143ffu, 0xf90013f5u, 0xa9034ff4u}, ringG, &ringG_stub_cont, "ringG"},
        {0x2d7ed80, {0xf940a329u, 0xb940392au, 0x34002ceau}, evdisp, &evdisp_stub_cont, "evdisp"},
        {0x2d81050, {0xd101c3ffu, 0xa9016ffcu, 0xa90267fau}, execon, &execon_stub_cont, "execon"},
        {0x2d813f0, {0xd10283ffu, 0xf9002bf9u, 0xa9065ff8u}, exeplay, &exeplay_stub_cont, "exeplay"},
        {0x2e17678, {0xd10143ffu, 0xa9015ff8u, 0xa90257f6u}, mixdev_stub, &mixdev_stub_cont, "mixdev"},      /* per-device mix pass: gates [0x430ae08] voices / [0x430ae44] */
        {0x2e150f4, {0xd101c3ffu, 0xa90267fau, 0xa9035ff8u}, voicecreate_stub, &voicecreate_stub_cont, "voicecreate"}, /* AK voice pool append */
        {0x2e14e14, {0xf81b0ff9u, 0xa9015ff8u, 0xa90257f6u}, vcbuild_stub, &vcbuild_stub_cont, "vcbuild"},        /* upstream: prepares a voice from a play request */
        {0x2e147fc, {0xd10143ffu, 0xa9015ff8u, 0xa90257f6u}, vccaller1_stub, &vccaller1_stub_cont, "vccaller1"},   /* calls voice create twice */
        {0x2e2547c, {0xd101c3ffu, 0xf9000bfbu, 0xa90267fau}, vccaller2_stub, &vccaller2_stub_cont, "vccaller2"},   /* calls voice create twice */
    };
    int lim = 99;
    { const char *v = getenv("PLA_AUDIO_SNDCHAIN_SITES"); if (v) { int x = atoi(v); if (x > 0) lim = x; } }
    int ok = 0;
    for (unsigned i = 0; i < sizeof sites / sizeof sites[0] && (int)i < lim; i++) {
        uint8_t *fn = base + sites[i].off;
        uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
        if (ex[0] != sites[i].w[0] || ex[1] != sites[i].w[1] || ex[2] != sites[i].w[2]) {
            fprintf(stderr, "[sndchain] %s prologue differs (%#x %#x %#x), skipped\n", sites[i].tag, ex[0], ex[1], ex[2]);
            continue;
        }
        *sites[i].cont = fn + 12;
        uint32_t pt[3] = {0, 0, 0};
        emit_adrp_br(pt, (uintptr_t)fn, (void *)sites[i].stub);
        memcpy(fn, pt, sizeof pt);
        sys_icache_invalidate(fn, 16);
        fprintf(stderr, "[sndchain] %s hooked at main+%#x\n", sites[i].tag, sites[i].off);
        ok++;
    }
    if (ok) fprintf(stderr, "[sndchain] %d chain sites hooked\n", ok);
    fflush(stderr);
}

void runtime_audio_cmd_probe_install(void);
void runtime_audio_cmd_probe_install(void) {
    if (!getenv("PLA_AUDIO_CMD_PROBE")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    if (!g_site_modbase) g_site_modbase = base;
    struct { uint32_t off; uint32_t w[3]; void (*stub)(void); void **cont; const char *tag; } sites[] = {
        {0x2ddcc24, {0xf81b0ff9u, 0xa9015ff8u, 0xa90257f6u}, cmd_evpost_stub, &cmd_evpost_stub_cont, "evpost"},
        {0x2d7de5c, {0xd101c3ffu, 0xa9016ffcu, 0xa90267fau}, cmd_alloc_stub, &cmd_alloc_stub_cont, "alloc"},
        {0x2dc98a8, {0xd101c3ffu, 0xf9000bfbu, 0xa90267fau}, cmd_nocmd_stub, &cmd_nocmd_stub_cont, "nocmd"},
        {0x2d7ead4, {0xd10443ffu, 0xa90b6ffcu, 0xa90c67fau}, cmd_ring_stub, &cmd_ring_stub_cont, "ring"},
        {0x2837394, {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, cmd_unreggo_stub, &cmd_unreggo_stub_cont, "reggo58"},
        {0x2837330, {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, cmd_reggo_stub, &cmd_reggo_stub_cont, "reggo50"},
        {0x85be0c, {0xf81d0ff5u, 0xa9014ff4u, 0xa9027bfdu}, cmd_factory_stub, &cmd_factory_stub_cont, "sndfactory"},
        {0x1dd82b8, {0xd100c3ffu, 0xf9000bf3u, 0xa9027bfdu}, cmd_sceneinit_stub, &cmd_sceneinit_stub_cont, "sndsceneinit"},
        {0x2d7ebf8, {0x2a1f03e8u, 0x79400349u, 0x11000d2au}, cmd_msgdone_stub, &cmd_msgdone_stub_cont, "msgdone"},
        {0x2d7ec5c, {0xd2c00088u, 0xf90043e8u, 0x390223ffu}, cmd_goevent_stub, &cmd_goevent_stub_cont, "goevent"},
        {0x2d7ecb0, {0xb940510au, 0x1100054au, 0xb900510au}, cmd_gofound_stub, &cmd_gofound_stub_cont, "gofound"},
        {0x2de0d64, {0xd10183ffu, 0xfd000be8u, 0xf9000ff9u}, cmd_action_stub, &cmd_action_stub_cont, "action"},
    };
    int ok = 0;
    int lim = 99;
    { const char *v = getenv("PLA_AUDIO_CMD_SITES"); if (v) { int x = atoi(v); if (x > 0) lim = x; } }
    for (unsigned i = 0; i < sizeof sites / sizeof sites[0] && (int)i < lim; i++) {
        uint8_t *fn = base + sites[i].off;
        uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
        if (ex[0] != sites[i].w[0] || ex[1] != sites[i].w[1] || ex[2] != sites[i].w[2]) {
            fprintf(stderr, "[cmd] %s prologue differs (%#x %#x %#x), skipped\n",
                    sites[i].tag, ex[0], ex[1], ex[2]);
            continue;
        }
        *sites[i].cont = fn + 12;
        uint32_t pt[3] = {0, 0, 0};
        emit_adrp_br(pt, (uintptr_t)fn, (void *)sites[i].stub);
        memcpy(fn, pt, sizeof pt);
        sys_icache_invalidate(fn, 16);
        fprintf(stderr, "[cmd] %s hooked at main+%#x\n", sites[i].tag, sites[i].off);
        ok++;
    }
    if (ok) fprintf(stderr, "[cmd] %d command-pipeline sites hooked\n", ok);
}
__attribute__((used)) static void *render_drive_cont;
static int g_rd_n;
static int (*g_rd_fn)(int);
void au_render_drive_note(void);
void au_render_drive_note(void) {
    int n = ++g_rd_n;
    int r = g_rd_fn ? g_rd_fn(1) : -1;
    if (n <= 3 || n % 200 == 0)
        fprintf(stderr, "[rdrive] #%d RenderAudio -> %d\n", n, r), fflush(stderr);
}
__attribute__((naked)) static void render_drive_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "bl _au_render_drive_note\n"
        "ldr x30, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        ".word 0xa9bd57f6\n" ".word 0xa9014ff4\n" ".word 0xa9027bfd\n"
        "adrp x16, _render_drive_cont@PAGE\n"
        "ldr x16, [x16, _render_drive_cont@PAGEOFF]\n"
        "br x16\n");
}
void runtime_audio_render_drive_install(void);
void runtime_audio_render_drive_install(void) {
    if (!getenv("PLA_AUDIO_RENDER_DRIVE")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    uint8_t *fn = base + 0x2e16ef8;
    uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
    if (ex[0] != 0xa9bd57f6u || ex[1] != 0xa9014ff4u || ex[2] != 0xa9027bfdu) {
        fprintf(stderr, "[rdrive] prologue differs (%#x %#x %#x), skipped\n", ex[0], ex[1], ex[2]); return;
    }
    g_rd_fn = (int (*)(int))(void *)(base + 0x2d75fe0);
    render_drive_cont = fn + 12;
    uint32_t pt[3] = {0, 0, 0};
    emit_adrp_br(pt, (uintptr_t)fn, (void *)render_drive_stub);
    memcpy(fn, pt, sizeof pt);
    sys_icache_invalidate(fn, 16);
    fprintf(stderr, "[rdrive] RenderAudio driven from main+0x2e16ef8 (%d Hz-ish)\n", 44);
}
void runtime_audio_site_probe_install(void);
void runtime_audio_site_probe_install(void) {
    if (!getenv("PLA_AUDIO_SITE_PROBE")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    if (!api_probe_base) api_probe_base = base;
    g_site_modbase = base;
    static const struct { uint64_t off; uint32_t w[3]; void *stub; void **cont; const char *tag; } sites[] = {
        {0x83fc18, {0xd10183ffu, 0xa90357f6u, 0xa9044ff4u}, (void *)site0_stub, &site0_stub_cont, "mgrctor"},
        {0x84e4c0, {0xd10483ffu, 0xf9006bfcu, 0xa90e5ff8u}, (void *)site1_stub, &site1_stub_cont, "chainstart"},
        {0x8405bc, {0xf81e0ff3u, 0xa9017bfdu, 0x910043fdu}, (void *)site2_stub, &site2_stub_cont, "step0"},
        {0x8405f8, {0xd100c3ffu, 0xa9014ff4u, 0xa9027bfdu}, (void *)site3_stub, &site3_stub_cont, "step1"},
        {0x8413a0, {0xf81e0ff3u, 0xa9017bfdu, 0x910043fdu}, (void *)site4_stub, &site4_stub_cont, "SCctor"},
        {0x841b58, {0xf81e0ff3u, 0xa9017bfdu, 0x910043fdu}, (void *)site5_stub, &site5_stub_cont, "framedrv"},
        {0x8470dc, {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, (void *)site6_stub, &site6_stub_cont, "ready"},
        {0x853f1c, {0xd10303ffu, 0xf90053f3u, 0xa90b7bfdu}, (void *)site7_stub, &site7_stub_cont, "gateload"},
        {0x2e35d80, {0x52800020u, 0xa9457bfdu, 0xa9444ff4u}, (void *)site8_stub, &site8_stub_cont, "sinkok"},
        {0x2e35df0, {0xa9bd57f6u, 0xa9014ff4u, 0xa9027bfdu}, (void *)site9_stub, &site9_stub_cont, "sinkdtor"},
        {0x2df4b14, {0xf81e0ff3u, 0xa9017bfdu, 0x910043fdu}, (void *)site10_stub, &site10_stub_cont, "rendertick"},
        {0x2df49e0, {0x6dbc23e9u, 0xf9000bf5u, 0xa9024ff4u}, (void *)site11_stub, &site11_stub_cont, "akmix"},
        {0x2e332ec, {0xf81e0ff3u, 0xa9017bfdu, 0x910043fdu}, (void *)site12_stub, &site12_stub_cont, "sndmgrcreate"},
        {0x2dc903c, {0xd100c3ffu, 0xa9014ff4u, 0xa9027bfdu}, (void *)site13_stub, &site13_stub_cont, "chainlast"},
        {0x2e33c9c, {0xa9bb67fau, 0xa9015ff8u, 0xa90257f6u}, (void *)site14_stub, &site14_stub_cont, "devsetup"},
        {0x2dac734, {0xa9bc5ff8u, 0xa90157f6u, 0xa9024ff4u}, (void *)site15_stub, &site15_stub_cont, "chainstepA"},
    };
    for (unsigned i = 0; i < sizeof sites / sizeof sites[0]; i++) {
        uint8_t *fn = base + sites[i].off;
        uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
        if (ex[0] != sites[i].w[0] || ex[1] != sites[i].w[1] || ex[2] != sites[i].w[2]) {
            fprintf(stderr, "[site] %s prologue differs (%#x %#x %#x), skipped\n", sites[i].tag, ex[0], ex[1], ex[2]);
            continue;
        }
        *sites[i].cont = fn + 12;
        uint32_t pt[3] = {0, 0, 0};
        emit_adrp_br(pt, (uintptr_t)fn, sites[i].stub);
        memcpy(fn, pt, sizeof pt);
        sys_icache_invalidate(fn, sizeof pt);
    }
    fprintf(stderr, "[site] probes installed (%zu sites)\n", sizeof sites / sizeof sites[0]);
    fflush(stderr);
}

/* PLA_AUDIO_FORCE_RENDER=1 (diagnostic): main+0x2df4b14 is the engine's per-tick gate — it renders the engine
   (vtable+0x70) only while the byte at this+8 is set, otherwise it primes the sink with silence (vtable+0x78).
   The flag never gets set in this port (see PLA_AUDIO_SITE_PROBE=1 rendertick needrender=0), so the engine is
   never asked to mix. NOP the guard at main+0x2df4b28 to let every tick render and see whether audio appears. */
void runtime_audio_force_render_install(void);
void runtime_audio_force_render_install(void) {
    if (!getenv("PLA_AUDIO_FORCE_RENDER")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    if (!api_probe_base) api_probe_base = base;
    uint8_t *fn = base + 0x2df4b14;
    uint32_t ex[6]; memcpy(ex, fn, sizeof ex);
    static const uint32_t want[6] = {0xf81e0ff3u, 0xa9017bfdu, 0x910043fdu, 0x39402008u, 0xaa0003f3u, 0x34000128u};
    if (memcmp(ex, want, sizeof want)) {
        fprintf(stderr, "[forcerender] prologue differs (%#x %#x %#x %#x %#x %#x), skipped\n",
                ex[0], ex[1], ex[2], ex[3], ex[4], ex[5]);
        return;
    }
    uint32_t nop = 0xd503201fu;                 /* cbz w8, <silence path> -> never taken */
    memcpy(fn + 20, &nop, sizeof nop);
    sys_icache_invalidate(fn, 24);
    fprintf(stderr, "[forcerender] render gate at main+0x2df4b28 forced on\n");
    fflush(stderr);
}

void runtime_audio_api_probe_install(void) {
    runtime_audio_force_render_install();
    runtime_audio_postcall_probe_install();
    runtime_audio_fill_probe_install();
    runtime_audio_site_probe_install();
    if (!getenv("PLA_AUDIO_API_PROBE")) return;
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) { fprintf(stderr, "[apiprobe] no nnMain\n"); return; }
    api_probe_base = (uint8_t *)nnmain - 0x2779f70ULL;
    if (!sr_probe_base) sr_probe_base = api_probe_base;
    static const struct { uint64_t off; const char *name; uint32_t w[3]; void *stub; void **cont; } sites[] = {
        {0x2813338, "setstate",  {0xd10203ffu, 0xf9001bf9u, 0xa9045ff8u}, (void *)api_setstate_stub,  &api_cont_setstate},
        {0x9b9db4,  "postevent", {0xd10303ffu, 0xa90957f6u, 0xa90a4ff4u}, (void *)api_postevent_stub, &api_cont_postevent},
        {0x2813898, "setrtpc",   {0xa9bf7bfdu, 0x910003fdu, 0xf940ac08u}, (void *)api_setrtpc_stub,   &api_cont_setrtpc},
        {0x2812fb8, "loadbank",  {0xf81d0ff5u, 0xa9014ff4u, 0xa9027bfdu}, (void *)api_loadbank_stub,  &api_cont_loadbank},
        {0x8417a8,  "gate",      {0xf81d0ff5u, 0xa9014ff4u, 0xa9027bfdu}, (void *)api_gate_stub,      &api_cont_gate},
        {0x2dcb224, "load.impl.sync",   {0xd10143ffu, 0xa9015ff8u, 0xa90257f6u}, (void *)api_impl1_stub, &api_cont_impl1},
        {0x2dcb9b8, "load.impl.async",  {0xd10143ffu, 0xa90257f6u, 0xa9034ff4u}, (void *)api_impl2_stub, &api_cont_impl2},
        {0x2dcb558, "prepare.impl.sync",{0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu}, (void *)api_impl3_stub, &api_cont_impl3},
        {0x2dcb664, "prepare.impl.async",{0xf81d0ff5u, 0xa9014ff4u, 0xa9027bfdu}, (void *)api_impl4_stub, &api_cont_impl4},
        {0x281304c, "wrap.a",           {0xf81c0ff7u, 0xa90157f6u, 0xa9024ff4u}, (void *)api_impl5_stub, &api_cont_impl5},
        {0x28130c4, "wrap.b",           {0xd10103ffu, 0xa9037bfdu, 0x9100c3fdu}, (void *)api_impl6_stub, &api_cont_impl6},
        {0x281356c, "postevent.pos",    {0xd102c3ffu, 0xa9056ffcu, 0xa90667fau}, (void *)api_postevent_pos_stub, &api_cont_pepos},
        {0x8dbe34,  "bankload.async",   {0xd10303ffu, 0xa90a4ff4u, 0xa90b7bfdu}, (void *)api_bankload_async_stub, &api_cont_bla},
        {0x8df3bc,  "bankload.sync",    {0xd10303ffu, 0xa90a4ff4u, 0xa90b7bfdu}, (void *)api_bankload_sync_stub,  &api_cont_bls},
        {0x2d8f2c8, "io.dispatcher",    {0xf81e0ffcu, 0xa9017bfdu, 0x910043fdu}, (void *)api_iodisp_stub,   &api_cont_iodisp},
        {0x2d90504, "bank.open",        {0xd10403ffu, 0xa90a6ffcu, 0xa90b67fau}, (void *)api_bankopen_stub, &api_cont_bankopen},
        {0x2e0e3bc, "bank.enqueue",     {0xa9bd57f6u, 0xa9014ff4u, 0xa9027bfdu}, (void *)api_enq_stub,      &api_cont_enq},
        {0x281629c, "postfail",         {0xd104c3ffu, 0xa90d6ffcu, 0xa90e67fau}, (void *)api_postfail_stub, &api_cont_postfail},
    };
    {   /* 0x283777c is a 3-word signature thunk (mov x0,x1; mov x1,x2; b 0x2d77440): verify, then patch */
        uint8_t *fn = api_probe_base + 0x283777cULL;
        uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
        if (ex[0] == 0xaa0103e0u && ex[1] == 0xaa0203e1u && ex[2] == 0x1414ff2fu) {
            api_bank_target = api_probe_base + 0x2d77440ULL;
            uint32_t pt[3] = {0, 0, 0};
            emit_adrp_br(pt, (uintptr_t)fn, (const void *)api_bankname_stub);
            memcpy(fn, pt, sizeof pt);
            sys_icache_invalidate(fn, sizeof pt);
            fprintf(stderr, "[apiprobe] LoadBank(name) thunk probe installed at %p\n", (void *)fn);
        } else fprintf(stderr, "[apiprobe] LoadBank thunk differs (%#x %#x %#x), skipped\n", ex[0], ex[1], ex[2]);
    }
    for (unsigned i = 0; i < sizeof sites / sizeof sites[0]; i++) {
        uint8_t *fn = api_probe_base + sites[i].off;
        uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
        if (memcmp(ex, sites[i].w, sizeof ex)) { fprintf(stderr, "[apiprobe] %s prologue differs (%#x %#x %#x), skipped\n", sites[i].name, ex[0], ex[1], ex[2]); continue; }
        *sites[i].cont = fn + 12;
        uint32_t pt[3] = {0, 0, 0};
        emit_adrp_br(pt, (uintptr_t)fn, sites[i].stub);
        memcpy(fn, pt, sizeof pt);
        sys_icache_invalidate(fn, sizeof pt);
        fprintf(stderr, "[apiprobe] %s probe installed at %p\n", sites[i].name, (void *)fn);
    }
}

/* Bank bootstrap (the audio fix): the game never issues a single LoadBank — not at boot, not in the field (only the
   32-byte ENV_BASE_AREA00 placeholder is loaded by the scene path). Without Init.bnk Wwise has no busses and every
   PostEvent returns 0, so the whole port is silent. The engine's own LoadBank(name, &id) is main+0x2d77440 (it hashes
   the lowercase basename, registers the name through the bank manager and queues the IO request that the audio thread
   opens); the game reaches it only through a thunk table, and that thunk is only ever reached by the PM-voice loader.
   So the host loads the banks the game needs at the first PostEvent — by then AK::SoundEngine::Init has run and the
   bank-manager IO thread is up. The patch stays resident (guest text is RX after load, so the original three prologue
   words are replayed by the stub instead of restored); the one-shot state flag makes later calls a pass-through.
   PLA_NO_BANK_BOOTSTRAP=1 disables it, PLA_BANK_LIST=Init.bnk,Common.bnk,... overrides the set. */
__attribute__((used)) uint64_t g_gameaudio_snapshot;
static const char *g_test_event; static uint64_t g_test_go;
static uint8_t *bank_bootstrap_entry;   /* patched PostEvent wrapper */
static uint8_t *bank_bootstrap_cont;    /* entry+12: the stub replays the overwritten prologue, then jumps here */
static int bank_bootstrap_state;        /* 0 = not run, 1 = ran */
static const char *const bank_bootstrap_default[] = { "Init.bnk", "Common.bnk", "UI.bnk", "BGM.bnk", "ME.bnk" };
void bank_bootstrap_run(void);
void bank_bootstrap_run(void) {
    if (bank_bootstrap_state) return;
    bank_bootstrap_state = 1;
    if (!bank_bootstrap_entry) return;
    int (*load_bank)(const char *, uint32_t *) = (int (*)(const char *, uint32_t *))(bank_bootstrap_entry - 0x2813338ULL + 0x2d77440ULL);
    const char *list = getenv("PLA_BANK_LIST");
    char buf[512];
    int n = 0;
    if (list && *list) { snprintf(buf, sizeof buf, "%s", list); }
    else {
        buf[0] = 0;
        for (unsigned i = 0; i < sizeof bank_bootstrap_default / sizeof bank_bootstrap_default[0]; i++) {
            if (i) strncat(buf, ",", sizeof buf - strlen(buf) - 1);
            strncat(buf, bank_bootstrap_default[i], sizeof buf - strlen(buf) - 1);
        }
    }
    for (char *p = buf; *p && n < 32; ) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        if (*p) {
            uint32_t id = 0;
            int r = load_bank(p, &id);
            fprintf(stderr, "[audbank] LoadBank(%s) -> %d id=%#x\n", p, r, id);
            fflush(stderr);
            n++;
        }
        if (!comma) break;
        p = comma + 1;
    }
    fprintf(stderr, "[audbank] bootstrap done (%d banks); PostEvent stub stays resident\n", n);
    /* PLA_TEST_EVENT=<name> (diagnostic): post the event ourselves, with game object 0 (AK_INVALID_GAME_OBJECT, the
       2D case), through AK::SoundEngine::PostEvent by name (main+0x2d77240 — name -> lowercase -> FNV-1a-32 ->
       0x2d77020).  If the engine can produce voices at all, the device handoff (main+0x2df49e0) must start firing;
       if it stays at zero, the engine has nothing to mix even for an event it just accepted. */
    if (g_test_event) {
        int (*post_by_name)(const char *, uint64_t, unsigned, uint64_t, uint64_t, uint64_t, unsigned, uint64_t) =
            (int (*)(const char *, uint64_t, unsigned, uint64_t, uint64_t, uint64_t, unsigned, uint64_t))
            (bank_bootstrap_entry - 0x2813338ULL + 0x2d77240ULL);
        uint64_t go = g_test_go;
        int r = post_by_name(g_test_event, go, 0, 0, 0, 0, 0, 0);
        fprintf(stderr, "[testev] PostEvent(\"%s\", go=%#llx) -> %d\n", g_test_event,
                (unsigned long long)go, r);
        fflush(stderr);
    }
    /* PLA_REG_GO=<id>[,<id>...] (diagnostic): feed the engine's deferred command for one API slot so we can tell
       whether "no audible voice" is simply "no game object was ever registered".  The thunk at main+0x2837328 is a
       game-side wrapper that validates the id (< 0x21) and enqueues command type 0xb into the engine ring; it loads
       the ring from [0x430a128] itself, so the only argument that matters is x1 (the game object id). */
    {
        const char *spec = getenv("PLA_REG_GO");
        if (spec && *spec) {
            int (*reg)(uint64_t, uint64_t, uint64_t) =
                (int (*)(uint64_t, uint64_t, uint64_t))(bank_bootstrap_entry - 0x2813338ULL + 0x2837328ULL);
            char buf[256];
            snprintf(buf, sizeof buf, "%s", spec);
            char *save = NULL;
            for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
                uint64_t go = strtoull(t, NULL, 0);
                int r = reg(0, go, 0);
                fprintf(stderr, "[reggo] slot+0x050(go=%llu) -> %d\n", (unsigned long long)go, r);
                fflush(stderr);
            }
            usleep(200000);
            uint8_t *mb = bank_bootstrap_entry - 0x2813338ULL;
            uint64_t gotbl = 0; memcpy(&gotbl, mb + 0x430a140, 8);
            uint32_t nb = 0; if (gotbl) memcpy(&nb, (void *)(uintptr_t)(gotbl + 0x38), 4);
            fprintf(stderr, "[reggo] game-object table %#llx buckets=%u\n", (unsigned long long)gotbl, nb);
            fflush(stderr);
        }
    }
    if (!api_probe_base) api_probe_base = bank_bootstrap_entry - 0x2813338ULL;
    if (getenv("PLA_AUDIO_APITAB_DUMP")) api_dump_table(g_gameaudio_snapshot);   /* diagnostics: off by default */
    if (getenv("PLA_AUDIO_APITAB_PROBE")) apitab_install(g_gameaudio_snapshot);
    {extern void runtime_audio_api_call_install(void); runtime_audio_api_call_install();}
    fflush(stderr);
}
/* The three words we overwrite at the PostEvent wrapper entry are `sub sp, sp, #0x80` / `str x25, [sp, #0x30]` /
   `stp x24, x23, [sp, #0x40]` — no PC-relative semantics, so replaying them here and jumping to entry+12 is exact.
   x0..x7 and x30 must survive the host call: the wrapper body from entry+12 still reads all of them. */
__attribute__((naked)) static void bank_bootstrap_stub(void) {
    __asm__ volatile(
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "stp x4, x5, [sp, #-16]!\n"
        "stp x6, x7, [sp, #-16]!\n"
        "str x30, [sp, #-16]!\n"
        "adrp x16, _g_gameaudio_snapshot@PAGE\n"
        "str x0, [x16, _g_gameaudio_snapshot@PAGEOFF]\n"
        "bl _bank_bootstrap_run\n"
        "ldr x30, [sp], #16\n"
        "ldp x6, x7, [sp], #16\n"
        "ldp x4, x5, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "sub sp, sp, #0x80\n"
        "str x25, [sp, #0x30]\n"
        "stp x24, x23, [sp, #0x40]\n"
        "adrp x16, _bank_bootstrap_cont@PAGE\n"
        "ldr x16, [x16, _bank_bootstrap_cont@PAGEOFF]\n"
        "br x16\n");
}

/* ---- Direct engine-entry invoker (PLA_AUDIO_CALL_API=main+0xXXX) -----------------------------
   The port is silent because the Wwise engine never mixes (akmix/handoff is never entered and the
   device float buffer stays zero) although banks, events, RTPC and the audio device are all fine,
   i.e. nothing drives the engine's render stage. This lets us call one engine entry point from a
   host thread at 60 Hz so the candidate renderer can be identified/validated empirically. */
static void *api_call_thread(void *arg) {
    int (*fn)(int) = (int (*)(int))(uintptr_t)arg;
    struct timespec ts = { 0, 16000000 };
    unsigned n = 0;
    for (;;) {
        int r = fn(1);
        if (++n <= 4 || (n & 0xff) == 0) {
            fprintf(stderr, "[apicall] #%u fn=main+0x%llx -> %d\n", n,
                    (unsigned long long)au_modoff((void *)fn), r);
            fflush(stderr);
        }
        nanosleep(&ts, NULL);
    }
    return NULL;
}
void runtime_audio_api_call_install(void);
void runtime_audio_api_call_install(void) {
    const char *env = getenv("PLA_AUDIO_CALL_API");
    if (!env || !*env) return;
    uint64_t off = strtoull(env, NULL, 0);
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) return;
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    void *fn = base + off;
    pthread_t th;
    if (pthread_create(&th, NULL, api_call_thread, fn) != 0) { fprintf(stderr, "[apicall] thread failed\n"); return; }
    pthread_detach(th);
    fprintf(stderr, "[apicall] calling main+0x%llx at 60 Hz\n", (unsigned long long)off);
}
void runtime_audio_bank_bootstrap_install(void) {
    if (getenv("PLA_NO_BANK_BOOTSTRAP")) return;
    g_apitab_on = getenv("PLA_AUDIO_APITAB_PROBE") != NULL;
    g_test_event = getenv("PLA_TEST_EVENT");
    if (getenv("PLA_TEST_GO")) g_test_go = strtoull(getenv("PLA_TEST_GO"), NULL, 0);
    void *nnmain = runtime_symbol("nnMain");
    if (!nnmain) { fprintf(stderr, "[audbank] no nnMain\n"); return; }
    uint8_t *base = (uint8_t *)nnmain - 0x2779f70ULL;
    uint8_t *fn = base + 0x2813338ULL;                 /* PostEvent wrapper: the earliest point with the engine up */
    uint32_t ex[3]; memcpy(ex, fn, sizeof ex);
    if (ex[0] != 0xd10203ffu || ex[1] != 0xf9001bf9u || ex[2] != 0xa9045ff8u) {
        fprintf(stderr, "[audbank] PostEvent prologue differs (%#x %#x %#x): bootstrap not installed\n", ex[0], ex[1], ex[2]);
        return;
    }
    bank_bootstrap_entry = fn;
    bank_bootstrap_cont = fn + 12;
    uint32_t pt[3] = {0, 0, 0};
    emit_adrp_br(pt, (uintptr_t)fn, (const void *)bank_bootstrap_stub);
    memcpy(fn, pt, sizeof pt);
    sys_icache_invalidate(fn, sizeof pt);
    fprintf(stderr, "[audbank] bank bootstrap installed at %p (entry+12 = %p)\n", (void *)fn, (void *)bank_bootstrap_cont);
    fflush(stderr);
    {extern void runtime_audio_render_drive_install(void); runtime_audio_render_drive_install();}
    {extern void runtime_audio_render_trace_install(void); runtime_audio_render_trace_install();}
    {extern void runtime_audio_render_body_install(void); runtime_audio_render_body_install();}   /* must patch before text goes RX */
    {extern void runtime_audio_cmd_probe_install(void); runtime_audio_cmd_probe_install();}   /* must patch before text goes RX */
    {extern void runtime_audio_sndchain_probe_install(void); runtime_audio_sndchain_probe_install();}   /* must patch before text goes RX */
    {extern void runtime_audio_req_probe_install(void); runtime_audio_req_probe_install();}   /* must patch before text goes RX */
}
