#include "world_survey.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mach/mach.h>

/* The TotK world survey reads TotK's actor and scene structures; Kirby has none of them. One Kirby diagnostic instead:
   "memdump <path>" writes main's writable data and bss (main+data_off .. main+size) to <path>, so two dumps can be
   compared to find the game state that changed between them. */
_Atomic int runtime_survey_pending;
const int32_t runtime_survey_sync[2] = {0, -1};   /* read by hle_nvn.c only while a survey is pending, which never happens here */
static Module *survey_main;
uint64_t dm_nvn_frame_count(void);
void runtime_survey_init(Module *main_module) { survey_main = main_module; }
/* "pin32 <value> <main offset> <offset>...": resolve the address as memchain does and keep writing the 32-bit value
   there every 2 ms (an A/B test of one game variable). */
#include <pthread.h>
#include <unistd.h>
static struct { volatile uint32_t *at; uint32_t v; } pins[8]; static int npins;
static void *pin_thread(void *arg) { (void)arg; for (;;) { for (int i = 0; i < npins; i++) *pins[i].at = pins[i].v; usleep(2000); } return NULL; }
static uint64_t chain_addr(const unsigned long long *off, int n) {
    uint64_t p = (uint64_t)(uintptr_t)survey_main->base + off[0];
    for (int i = 1; i < n; i++) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)p, 8); if (!v) return 0; p = v + off[i]; }
    return p;
}
int runtime_survey_command(const char *line) {
    /* "watch <len> <main offset> <offset>...": arm the host write watchpoint (gfx_bridge.c) on that resolved address */
    if (!strncmp(line, "watch ", 6) && survey_main) {
        unsigned long long len, off[8]; int n = 0, used = 0, u;
        if (sscanf(line + 6, "%llx%n", &len, &used) != 1) return 1;
        const char *q = line + 6 + used;
        while (n < 8 && sscanf(q, " %llx%n", &off[n], &u) == 1) { q += u; n++; }
        uint64_t a = n ? chain_addr(off, n) : 0; if (!a) return 1;
        extern void runtime_watch_set(uintptr_t addr, size_t len); runtime_watch_set((uintptr_t)a, (size_t)len);
        fprintf(stderr, "[kirby] watch %#llx bytes at %#llx\n", len, (unsigned long long)a);
        return 1;
    }
    if (!strncmp(line, "pin32 ", 6) && survey_main && npins < 8) {
        unsigned long long v, off[8]; int n = 0, used = 0, u;
        if (sscanf(line + 6, "%llx%n", &v, &used) != 1) return 1;
        const char *q = line + 6 + used;
        while (n < 8 && sscanf(q, " %llx%n", &off[n], &u) == 1) { q += u; n++; }
        uint64_t a = n ? chain_addr(off, n) : 0; if (!a) return 1;
        pins[npins].at = (volatile uint32_t *)(uintptr_t)a; pins[npins].v = (uint32_t)v;
        if (npins++ == 0) { pthread_t t; pthread_create(&t, NULL, pin_thread, NULL); pthread_detach(t); }
        fprintf(stderr, "[kirby] pin32 F%llu %#llx at %#llx\n", (unsigned long long)dm_nvn_frame_count(), v, (unsigned long long)a);
        return 1;
    }
    /* "memchain <path> <len> <main offset> <offset>...": p = main+<main offset>; each further offset is p = *(u64 *)p + offset;
       then <len> bytes at p go to <path>. */
    if (!strncmp(line, "memchain ", 9) && survey_main) {
        char path[1024]; unsigned long long len, off[8]; int n = 0, used = 0;
        if (sscanf(line + 9, "%1023s %llx%n", path, &len, &used) != 2) return 1;
        const char *q = line + 9 + used; int u;
        while (n < 8 && sscanf(q, " %llx%n", &off[n], &u) == 1) { q += u; n++; }
        if (!n || len > (64u << 20)) return 1;
        uint64_t p = (uint64_t)(uintptr_t)survey_main->base + off[0];
        for (int i = 1; i < n; i++) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)p, 8); if (!v) { fprintf(stderr, "[kirby] memchain: null at step %d\n", i); return 1; } p = v + off[i]; }
        FILE *f = fopen(path, "wb"); if (!f) return 1;
        fwrite((const void *)(uintptr_t)p, 1, len, f); fclose(f);
        fprintf(stderr, "[kirby] memchain F%llu %#llx bytes at %#llx -> %s\n", (unsigned long long)dm_nvn_frame_count(), len, (unsigned long long)p, path);
        return 1;
    }
    if (strncmp(line, "memdump ", 8) || !survey_main) return 0;
    char path[1024]; snprintf(path, sizeof path, "%s", line + 8); path[strcspn(path, "\r\n")] = 0;
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[kirby] memdump: cannot open %s\n", path); return 1; }
    size_t n = survey_main->size - survey_main->data_off;
    fwrite(survey_main->base + survey_main->data_off, 1, n, f); fclose(f);
    fprintf(stderr, "[kirby] memdump F%llu main+%#llx..+%#zx -> %s\n", (unsigned long long)dm_nvn_frame_count(), (unsigned long long)survey_main->data_off, survey_main->size, path);
    return 1;
}
void runtime_survey_tick(uint64_t frame) { (void)frame; }
/* Diagnostic KIRBY_SNAP=<lo>-<hi>:<dir>: at each present of frames lo..hi, dump 0xa0000 bytes of the render system
   object ([[[main+0xa61e58]+8]+8]+0xac8) to <dir>/s<frame>.bin, frame-synchronous. */
void runtime_kirby_snapshot(uint64_t frame) {
    static int init; static uint64_t lo, hi; static char dir[512];
    if (!init) { init = 1; const char *e = getenv("KIRBY_SNAP"); unsigned long long a, b; int n = 0;
        if (e && sscanf(e, "%llu-%llu:%n", &a, &b, &n) == 2 && n) { lo = a; hi = b; snprintf(dir, sizeof dir, "%s", e + n); } }
    if (!hi || frame < lo || frame > hi || !survey_main) return;
    /* KIRBY_SNAP_CHAIN=loop: the frame controller [[main+0xa61e58]+8] instead of the render system */
    const char *which = getenv("KIRBY_SNAP_CHAIN");
    const unsigned long long off[] = {0xa61e58, 0x8, 0x8, 0xac8, 0x0}, loop[] = {0xa61e58, 0x8, 0x0};
    uint64_t p = which && !strcmp(which, "loop") ? chain_addr(loop, 3) : chain_addr(off, 5); if (!p) return;
    /* KIRBY_SNAP_SPAN=<before>,<size> (hex): a wider window around the object, readable pages only */
    static uint64_t before, size = 0xa0000; static int span_init;
    if (!span_init) { span_init = 1; const char *e = getenv("KIRBY_SNAP_SPAN"); unsigned long long a, b; if (e && sscanf(e, "%llx,%llx", &a, &b) == 2) { before = a; size = b; } }
    { static int said; if (!said++) fprintf(stderr, "[kirby] snapshot object at %#llx, file starts at %#llx\n", (unsigned long long)p, (unsigned long long)((p - before) & ~0x3fffull)); }
    char path[600]; snprintf(path, sizeof path, "%s/s%llu.bin", dir, (unsigned long long)frame);
    FILE *f = fopen(path, "wb"); if (!f) return;
    for (uint64_t q = (p - before) & ~0x3fffull; q < p - before + size; q += 0x4000) {
        static uint8_t zero[0x4000]; vm_size_t got = 0; uint8_t page[0x4000];
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)q, 0x4000, (vm_address_t)page, &got) == KERN_SUCCESS && got == 0x4000) fwrite(page, 1, 0x4000, f);
        else fwrite(zero, 1, 0x4000, f);
    }
    fclose(f);
}
void runtime_survey_cancel_user(void) {}
