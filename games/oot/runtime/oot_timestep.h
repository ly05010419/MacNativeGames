/* OOT update 131072: native VFR uses 60 / configured FPS even when a frame
   actually took longer. Its framework already records the elapsed system ticks
   at +0x100 (written by main+0x346c7ec, 31,250,000 Hz). Use that duration at
   BOTH ratio producers, before the game's VFR manager applies its own scales.
   The logic producer's original 30/60 selector, pause flags, task masks and
   per-domain scales remain native. No global system/NVN clock is changed.

   These are Mac functional game patches, never included in an iOS export.
   The veneers preserve NZCV, x16/x17 and q16/q17, and only replace the FDIV
   destination. A zero initial elapsed value executes the original FDIV.
   Page layout: pinned seconds (+0, zero = native measured duration), framework
   pointer (+8), logic-update count (+16), accumulated logic seconds (+24),
   latest input/logic ratios (+32/+36). The latter fields support live checks.
   framestep <ms> pins seconds for diagnostics; framestep 0 restores real time. */
static _Atomic uint32_t *oot_step_pin;
void runtime_frame_step_pin(double ms) {
    if (!oot_step_pin) return;
    float seconds = (float)(ms / 1000.0); uint32_t bits;
    memcpy(&bits, &seconds, sizeof bits);
    atomic_store_explicit(oot_step_pin, bits, memory_order_relaxed);
    fprintf(stderr, "[oot] time step: %s (pin %.6f ms)\n", ms ? "diagnostic fixed duration" : "framework elapsed ticks", ms);
}
static uint32_t oot_step_branch(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)to - (int64_t)from;
    if ((d & 3) || d < -(1LL << 27) || d >= (1LL << 27))
        runtime_fail("OOT time-step branch out of range");
    return 0x14000000 | ((uint32_t)(d / 4) & 0x03ffffff);
}
static void patch_oot_timestep(Module *m) {
    if (getenv("TOTK_EXPORT_IOS")) return;
    const char *e = getenv("OOT_REALTIME");
    if (e && !strcmp(e, "0")) {
        fprintf(stderr, "[oot] native configured-FPS time step (OOT_REALTIME=0)\n");
        return;
    }
    static const uint32_t input_context[] = {
        0xbd42ca60,0x52a84e08,0x910be260,0x1e270101,0xf0008f88,
        0x913f2108,0x5e21d800,0x1e201820,0xbd000100
    };
    static const uint32_t logic_context[] = {
        0xbd42caa0,0xf9400114,0x39493ea8,0xb94d3a69,0x5e21d800,
        0x7200011f,0x52a84e08,0x1e270101,0x9134e268,0xb82a1108,
        0x1e220c21,0x1e201828
    };
    /* stp x16,x17; stp q16,q17; adrp/add page; optional pin; otherwise
       scvtf elapsed; multiply by 1/31250000; multiply original numerator;
       record; restore; branch back. Literal pools stay in the RX text page. */
    /* The measured duration is capped (2026-11-08): seconds = min(ticks / 31250000, cap), cap 1/30 s by default, the
       game's own slowest native step (its 30/60 selector). Unbounded, a 2.3 s loading hitch became one 2.3 s physics
       step; Havok's contact buffers for it (148 MiB of short-lived blocks in one second, claude/oot-net-bypass hr2)
       overran its 49 MiB limit (Q+0x60) and the game died (null header store at main+0xa98e78, or the SDK abort).
       With the original fixed step (OOT_REALTIME=0) the burst is absent (hr3). Above 30 fps nothing changes; below it
       the game slows down as on the console. OOT_REALTIME_MAX_MS=<ms> overrides the cap for diagnostics.
       ldr s17, cap; fmin s16, s16, s17 sit before the numerator multiply, so a pinned duration is capped too. */
    uint32_t input[] = {
        0xa9bd47f0,0xad00c7f0,0x90000011,0x91000231,0xbd400230,0x1e260210,
        0x350000d0,0xf9408270,0xb4000110,0x9e220210,0x1c000191,0x1e310a10,
        0x1c000171,0x1e315a10,0x1e300820,0x14000002,0x1e201820,0xf9000633,
        0xbd002220,0xad40c7f0,0xa8c347f0,0x14000000,0x3309705f,0x3d088889,
        0x11111111,0x3f911111
    };
    uint32_t logic[] = {
        0xa9bd47f0,0xad00c7f0,0x90000011,0x91000231,0xbd400230,0x1e260210,
        0x350000d0,0xf94082b0,0xb4000110,0x9e220210,0x1c0002b1,0x1e310a10,
        0x1c000291,0x1e315a10,0x1e300828,0x14000002,0x1e201828,0xf9000635,
        0xbd002628,0xf9400a30,0x91000610,0xf9000a30,0x1e22c110,0x5c000171,
        0x1e710a10,0xfd400e31,0x1e712a10,0xfd000e30,0xad40c7f0,0xa8c347f0,
        0x14000000,0x3309705f,0x3d088889,0xd503201f,0x11111111,0x3f911111
    };
    enum { INPUT_CAP = 23, LOGIC_CAP = 32 };
    {   const char *mx = getenv("OOT_REALTIME_MAX_MS");
        float cap = 1.0f / 30.0f;
        if (mx && *mx) { char *e = NULL; double ms = strtod(mx, &e); if (!e || *e || !(ms > 0 && ms <= 1000)) runtime_fail("OOT_REALTIME_MAX_MS must be in (0, 1000]"); cap = (float)(ms / 1000.0); }
        uint32_t bits; memcpy(&bits, &cap, sizeof bits); input[INPUT_CAP] = bits; logic[LOGIC_CAP] = bits;
        fprintf(stderr, "[oot] time step capped at %.3f ms a frame%s\n", cap * 1000.0, mx && *mx ? " (OOT_REALTIME_MAX_MS)" : "");
    }
    uint64_t page = (m->file_size + 0x3fff) & ~0x3fffULL;
    uint64_t stub = (m->text_end + 15) & ~15ULL;
    uint64_t rx_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    if (m->text_end != 0x3c461d0 || m->file_size < 0x4e94b28 ||
        page + 0x4000 > m->size || stub + sizeof input + sizeof logic > rx_end ||
        memcmp(m->base + 0x3b4eadc, input_context, sizeof input_context) ||
        memcmp(m->base + 0x6e6c98, logic_context, sizeof logic_context))
        runtime_fail("OOT time-step sites differ from verified update 131072");
    for (uint64_t k = stub; k < stub + sizeof input + sizeof logic; k++)
        if (m->base[k]) runtime_fail("OOT time-step text padding already in use");
    for (uint64_t k = page; k < page + 40; k++)
        if (m->base[k]) runtime_fail("OOT time-step data page already in use");
    uint32_t *code[] = { input, logic };
    const uint64_t sites[] = { 0x3b4eaf8, 0x6e6cc4 };
    const unsigned lengths[] = { sizeof input, sizeof logic }, returns[] = { 21, 30 };
    for (unsigned k = 0; k < 2; k++) {
        int64_t pages = ((int64_t)page - (int64_t)((stub + 8) & ~4095ULL)) / 4096;
        code[k][2] = 0x90000011 | (((uint32_t)pages & 3) << 29) |
                     ((((uint32_t)pages >> 2) & 0x7ffff) << 5);
        code[k][3] = 0x91000231 | ((page & 4095) << 10);
        code[k][returns[k]] = oot_step_branch(stub + 4 * returns[k], sites[k] + 4);
        uint32_t jump = oot_step_branch(sites[k], stub);
        memcpy(m->base + stub, code[k], lengths[k]);
        memcpy(m->base + sites[k], &jump, sizeof jump);
        stub += lengths[k];
    }
    oot_step_pin = (_Atomic uint32_t *)(void *)(m->base + page);
    fprintf(stderr, "[oot] native VFR uses framework elapsed ticks (31250000 Hz); input and logic; state main+0x%llx\n", (unsigned long long)page);
}
