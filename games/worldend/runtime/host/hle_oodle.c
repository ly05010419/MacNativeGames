// The game's own decoder, for the offline shader tools. Unreal Engine games compress each shader in their shader code archives with Oodle (Octopath Traveler 0: every entry of the
// two .ushaderbytecode files in pakchunk0-Switch.pak, block header 8C 0A = Mermaid), Game Freak's Trinity engine every file of its packs (Pokémon Legends: Z-A: arc/data.trpfs, compression
// type 3), PlatinumGames' engine the entries of its PKZ archives (Bayonetta 3: OodleKraken, block header 8C 06), and DRAGON QUEST XI S (Unreal Engine 4.18) the files of its paks with LZ5 2.0
// (pak compression method 3; each block starts with the LZ5 level, 0x2d). Shaders are translated offline
// only and Oodle's decoder is not open source, so they are decoded on the Mac with the copy statically linked into main (LZ5 the same way, so the tools need no decoder of their own), in one
// of two modes:
// DM_OODLE_JOB=<job file> runs the decoder on every record right before nnMain, writes <job file>.out and exits (the NSP script, Unreal);
// DM_OODLE_SERVE=1 answers requests on stdin / stdout from the guest main thread before the SDK starts, until stdin ends (shader_split.py's workers, Trinity: the packs are decoded file by file
// while they are scanned, about 4.7 GB, so they are never written out). Request: u64 compressed length, u64 decompressed length, the compressed bytes; answer: u64 bytes decoded (0 on failure),
// then those bytes.
// Job file: "DMOJ", u32 count, then per record u32 compressed length, u32 raw length and the compressed bytes. Output: "DMOR", u32 count, then per record u32 decoded length
// (0 = failed) and the decoded bytes.
#include "hle.h"
#include <unistd.h>

// OodleLZ_Decompress is found in main by the name its profiler scope passes: the one function whose code takes the address of the NUL-terminated string "OodleLZ_Decompress" (adrp + add),
// with its start from the module's .eh_frame_hdr. The string belongs to the Oodle library itself, so the rule holds whatever the engine: Octopath Traveler 0 1.0.5 (Unreal) main+0x4f77e10,
// Pokemon Legends: Z-A 2.0.2 main+0x1a9c9e0, each the only function referencing it (twice)
static uint64_t eh_read(const uint8_t *b, uint64_t *o, int enc, uint64_t ehs) {   // one .eh_frame_hdr value (formats udata4 / sdata4 / udata8, relative to itself or to the table)
    uint64_t v; int fmt = enc & 0xF, app = enc & 0x70, at = (int)*o;
    if (fmt == 0x3) { uint32_t x; memcpy(&x, b + at, 4); v = x; *o += 4; }
    else if (fmt == 0xB) { int32_t x; memcpy(&x, b + at, 4); v = (uint64_t)(int64_t)x; *o += 4; }
    else if (fmt == 0x4) { memcpy(&v, b + at, 8); *o += 8; }
    else die("DM_OODLE_JOB: .eh_frame_hdr encoding %#x not handled", enc);
    return app == 0x10 ? v + (uint64_t)at : app == 0x30 ? v + ehs : v;
}
static uint64_t func_start(const module_t *m, uint64_t off) {   // start of the function holding off (the table is sorted by start)
    const uint8_t *b = m->base; uint32_t m0; int32_t eh; memcpy(&m0, b + 4, 4); memcpy(&eh, b + m0 + 16, 4);
    if (memcmp(b + m0, "MOD0", 4)) die("DM_OODLE_JOB: main has no MOD0");
    uint64_t ehs = m0 + (int64_t)eh, o = ehs + 4; int penc = b[ehs + 1], cenc = b[ehs + 2], tenc = b[ehs + 3];
    eh_read(b, &o, penc, ehs); uint64_t n = eh_read(b, &o, cenc, ehs), best = 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t s = eh_read(b, &o, tenc, ehs); eh_read(b, &o, tenc, ehs); if (s <= off && s > best) best = s; }
    return best;
}
static uint64_t func_next(const module_t *m, uint64_t off) {   // start of the function after the one holding off (the text end when there is none)
    const uint8_t *b = m->base; uint32_t m0; int32_t eh; memcpy(&m0, b + 4, 4); memcpy(&eh, b + m0 + 16, 4);
    uint64_t ehs = m0 + (int64_t)eh, o = ehs + 4, best = m->text_end; int penc = b[ehs + 1], cenc = b[ehs + 2], tenc = b[ehs + 3];
    eh_read(b, &o, penc, ehs); uint64_t n = eh_read(b, &o, cenc, ehs);
    for (uint64_t i = 0; i < n; i++) { uint64_t s = eh_read(b, &o, tenc, ehs); eh_read(b, &o, tenc, ehs); if (s > off && s < best) best = s; }
    return best;
}
// The functions whose code takes the address of a NUL-terminated string of main's read-only data (adrp + add) that holds needle: with whole, the string is needle; else the needle is anywhere
// in it, and the address taken is the string's start. Up to max distinct functions into fns, in address order; returns how many
static int str_fns(const module_t *m, const char *needle, int whole, uint64_t *fns, int max) {
    const uint8_t *b = m->base, *ro = b + m->ro_off, *re = b + m->data_off; size_t len = strlen(needle);
    uint64_t strs[16]; int ns = 0, nf = 0;
    for (const uint8_t *p = ro; ns < 16 && (p = memmem(p, (size_t)(re - p), needle, len)); p += len) {
        const uint8_t *s = p; while (s > ro && s[-1]) s--;
        if (!whole || (s == p && p[len] == 0)) strs[ns++] = (uint64_t)(s - b);
    }
    for (uint64_t pc = 0; ns && pc + 4 <= m->text_end; pc += 4) {
        uint32_t w; memcpy(&w, b + pc, 4);
        if ((w & 0x9F000000) != 0x90000000) continue;   // adrp xd, page
        int64_t imm = (int64_t)(((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)); if (imm & (1 << 20)) imm -= 1 << 21;
        uint64_t page = (pc & ~0xFFFull) + (uint64_t)(imm << 12);
        for (int k = 1; k < 8 && pc + 4 * k + 4 <= m->text_end; k++) {   // the add that completes it, within a few instructions
            uint32_t w2; memcpy(&w2, b + pc + 4 * k, 4);
            if ((w2 & 0xFF800000) != 0x91000000 || ((w2 >> 5) & 31) != (w & 31)) continue;   // add xd, xn = xd of the adrp, #imm
            uint64_t a = page + ((w2 >> 10) & 0xFFF);
            for (int s = 0; s < ns; s++) if (a == strs[s]) {
                uint64_t f = func_start(m, pc + 4 * k); int seen = 0;
                for (int i = 0; i < nf; i++) seen |= fns[i] == f;
                if (!seen && nf < max) fns[nf++] = f;
            }
            break;
        }
    }
    for (int i = 1; i < nf; i++) for (int j = i; j > 0 && fns[j - 1] > fns[j]; j--) { uint64_t t = fns[j]; fns[j] = fns[j - 1]; fns[j - 1] = t; }
    return nf;
}
static uint64_t oodle_find(const module_t *m) {   // 0: main has no "OodleLZ_Decompress" name string
    uint64_t fns[4]; int nfn = str_fns(m, "OodleLZ_Decompress", 1, fns, 4);
    if (nfn > 1) die("DM_OODLE_JOB: %d functions reference \"OodleLZ_Decompress\" in main (expected exactly 1)", nfn);
    return nfn ? fns[0] : 0;
}

// Oodle's streaming decoder, for a main that has no "OodleLZ_Decompress" name (Bayonetta 3 1.2.0, PlatinumGames: it reads its PKZ archives only through the streaming API, main+0xcbc698 /
// +0xcbc7f8, so OodleLZ_Decompress was never linked in). The four functions come from Oodle's oodlelzcompressors.cpp, whose path two of them pass to its error handler with a line number:
// OodleLZDecoder_MemorySizeNeeded, the one of the two that also takes the message "OODLE ERROR : OodleLZ_Decompress: asked to decode Legacy compressor", and OodleLZDecoder_DecodeSome,
// the other; between them lie OodleLZDecoder_Create (right after the first) and OodleLZDecoder_Destroy (right before the second), in the library's own order (main+0x12fefd0, +0x12ff110,
// +0x12ff2d0, +0x12ff2f0 there). Checked as far as the code allows: exactly two functions reference the path, the legacy message is in the first, and exactly two functions lie between them
typedef int32_t (*lzd_size_fn)(int32_t compressor, int64_t raw_len);
typedef void *(*lzd_create_fn)(int32_t compressor, int64_t raw_len, void *mem, int64_t mem_size);
typedef void (*lzd_destroy_fn)(void *dec);
// OodleLZDecoder_DecodeSome(decoder, out, decBuf, decBufPos, decBufferSize, decBufAvail, compPtr, compAvail, fuzzSafe, checkCRC, verbosity, threadPhase): arguments 9-12 on the stack, 64-bit
// here so the Apple and AAPCS64 stack layouts agree; out = {s32 decoded, s32 compressed bytes used, s32 current quantum's raw length, s32 its compressed length}
typedef int32_t (*lzd_some_fn)(void *dec, int32_t *out, void *buf, int64_t pos, int64_t size, int64_t avail, const void *comp, int64_t comp_avail, int64_t fuzz, int64_t crc, int64_t verb, int64_t phase);
static struct { lzd_size_fn size; lzd_create_fn create; lzd_destroy_fn destroy; lzd_some_fn some; } g_lzd;
static uint64_t lzd_find(const module_t *m) {
    uint64_t src[4], leg[4]; int ns = str_fns(m, "oodlelzcompressors.cpp", 0, src, 4), nl = str_fns(m, "OODLE ERROR : OodleLZ_Decompress: asked to decode Legacy compressor", 0, leg, 4);
    if (ns != 2 || nl != 1 || leg[0] != src[0])
        die("DM_OODLE_JOB: main has no \"OodleLZ_Decompress\" name and no streaming decoder to find (%d functions reference oodlelzcompressors.cpp, %d the legacy compressor message)", ns, nl);
    uint64_t create = func_next(m, src[0]), destroy = func_next(m, create);
    if (func_next(m, destroy) != src[1]) die("DM_OODLE_JOB: Oodle's streaming decoder at main+%#llx / +%#llx: not two functions between them", (unsigned long long)src[0], (unsigned long long)src[1]);
    const uint8_t *b = m->base;
    g_lzd.size = (lzd_size_fn)(void *)(b + src[0]); g_lzd.create = (lzd_create_fn)(void *)(b + create); g_lzd.destroy = (lzd_destroy_fn)(void *)(b + destroy); g_lzd.some = (lzd_some_fn)(void *)(b + src[1]);
    return src[1];
}
// The compressor an Oodle stream names in its first block header (byte 0: 0x8C or 0xCC; byte 1 low 7 bits: the decoder: 6 Kraken, 10 Mermaid / Selkie, 12 Leviathan) as the API's
// OodleLZ_Compressor (Kraken 8, Mermaid 9, Leviathan 13), which Create sizes its memory for; -1 when it is none of them
static int32_t lzd_compressor(const uint8_t *c, int64_t cl) {
    if (cl < 2 || (c[0] & 0x0F) != 0x0C) return -1;
    switch (c[1] & 0x7F) { case 6: return 8; case 10: return 9; case 12: return 13; default: return -1; }
}
static int64_t lzd_run(const void *c, int64_t cl, void *r, int64_t rl, void *scratch, int64_t scratch_n) {
    int32_t comp = lzd_compressor(c, cl); if (comp < 0) return -1;
    int64_t need = g_lzd.size(comp, rl); void *mem = scratch, *own = NULL;
    if (need <= 0) return -1;
    if (need + 64 > scratch_n) { if (!(own = malloc((size_t)need + 64))) return -1; mem = own; }
    void *dec = g_lzd.create(comp, rl, mem, need + 64);   // Create keeps 16 bytes for its own alignment
    if (!dec) { free(own); return -1; }
    int64_t pos = 0, cpos = 0;
    while (pos < rl) {
        int32_t out[4] = {0};
        if (!g_lzd.some(dec, out, r, pos, rl, rl - pos, (const uint8_t *)c + cpos, cl - cpos, 1, 0, 0, 3) || (out[0] <= 0 && out[1] <= 0)) break;   // fuzz safe, no CRC check, silent, unthreaded
        pos += out[0]; cpos += out[1];
    }
    g_lzd.destroy(dec); free(own);
    return pos;
}

// OodleLZ_Decompress(compBuf, compBufSize, rawBuf, rawLen, fuzzSafe, checkCRC, verbosity, decBufBase, decBufSize, fpCallback, callbackUserData, decoderMemory, decoderMemorySize, threadPhase).
// Arguments 9-14 go on the stack; all are 64-bit here so the Apple and AAPCS64 stack layouts agree (Apple packs smaller stack arguments)
typedef int64_t (*oodle_fn)(const void *, int64_t, void *, int64_t, int64_t, int64_t, int64_t, void *, int64_t, void *, void *, void *, int64_t, int64_t);
// LZ5_decompress_safe(source, dest, compressedSize, maxDecompressedSize) → bytes decoded, negative on error (LZ5 2.0, later renamed Lizard: a level byte, then blocks of streams, some
// Huffman-coded); it needs no memory of its own. main exports it by name (DRAGON QUEST XI S 1.0.4: main+0x4ab3e00, LZ5_versionNumber 20001), so the export is the rule
typedef int32_t (*lz5_fn)(const void *, void *, int32_t, int32_t);

// The decoder main has: LZ5 when main exports LZ5_decompress_safe, else Oodle (the one function that references its name)
static struct { const char *name; oodle_fn oodle; lz5_fn lz5; uint64_t off; } g_dec;
static void dec_find(const char *mode) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        void *p = module_sym(&g_mod[i], "LZ5_decompress_safe");
        if (p) { g_dec.name = "LZ5_decompress_safe"; g_dec.lz5 = (lz5_fn)p; g_dec.off = (uint64_t)((uint8_t *)p - g_mod[i].base); return; }
        if ((g_dec.off = oodle_find(&g_mod[i]))) { g_dec.oodle = (oodle_fn)(void *)(g_mod[i].base + g_dec.off); g_dec.name = "OodleLZ_Decompress"; return; }
        g_dec.off = lzd_find(&g_mod[i]); g_dec.name = "OodleLZDecoder_DecodeSome"; return;
    }
    die("%s: no main module", mode);
}
static int64_t dec_run(const void *c, int64_t cl, void *r, int64_t rl, void *scratch, int64_t scratch_n) {
    if (g_dec.lz5) return cl > INT32_MAX || rl > INT32_MAX ? -1 : g_dec.lz5(c, r, (int32_t)cl, (int32_t)rl);
    if (!g_dec.oodle) return lzd_run(c, cl, r, rl, scratch, scratch_n);
    return g_dec.oodle(c, cl, r, rl, 1, 0, 0, NULL, 0, NULL, NULL, scratch, scratch_n, 3);   // fuzz safe, no CRC check, silent, unthreaded
}

static uint32_t rd32(FILE *f) { uint32_t v = 0; if (fread(&v, 4, 1, f) != 1) die("DM_OODLE_JOB: the job file ends early"); return v; }

void dm_oodle_job_maybe(void) {
    const char *job = getenv("DM_OODLE_JOB");
    if (!job || !*job) return;
    dec_find("DM_OODLE_JOB");
    LOG("DM_OODLE_JOB: %s at main+%#llx", g_dec.name, (unsigned long long)g_dec.off);
    FILE *in = fopen(job, "rb"); if (!in) die("DM_OODLE_JOB: cannot open %s", job);
    if (rd32(in) != 0x4a4f4d44) die("DM_OODLE_JOB: %s is not a job file", job);
    uint32_t n = rd32(in);
    char outp[1024]; snprintf(outp, sizeof outp, "%s.out", job);
    FILE *out = fopen(outp, "wb"); if (!out) die("DM_OODLE_JOB: cannot write %s", outp);
    uint32_t hdr[2] = {0x524f4d44, n}; fwrite(hdr, 4, 2, out);
    size_t scratch_n = 16u << 20; void *scratch = malloc(scratch_n);   // the decoder's work memory, so Oodle never calls its allocator
    uint32_t bad = 0; uint64_t raw_total = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cl = rd32(in), rl = rd32(in);
        uint8_t *c = malloc(cl ? cl : 1), *r = malloc(rl ? rl : 1);
        if (fread(c, 1, cl, in) != cl) die("DM_OODLE_JOB: record %u ends early", i);
        int64_t got = dec_run(c, cl, r, rl, scratch, (int64_t)scratch_n);
        uint32_t ol = got == (int64_t)rl ? rl : 0;
        if (!ol && bad++ < 8) LOG("[RED] DM_OODLE_JOB: record %u (%u → %u bytes) failed (decoder returned %lld)", i, cl, rl, (long long)got);
        fwrite(&ol, 4, 1, out); if (ol) fwrite(r, 1, ol, out);
        raw_total += ol; free(c); free(r);
    }
    fclose(in); fclose(out); free(scratch);
    LOG("DM_OODLE_JOB: %u records decoded with the game's %s (%u failed, %.1f MB out) → %s", n - bad, g_dec.name, bad, raw_total / 1048576.0, outp);
    fflush(stderr); _exit(bad ? 1 : 0);
}

static int read_all(int fd, void *p, size_t n) {
    for (uint8_t *q = p; n; ) { ssize_t k = read(fd, q, n); if (k <= 0) return 0; q += k; n -= (size_t)k; }
    return 1;
}
static void write_all(int fd, const void *p, size_t n) {
    for (const uint8_t *q = p; n; ) { ssize_t k = write(fd, q, n); if (k <= 0) die("DM_OODLE_SERVE: answer write failed"); q += k; n -= (size_t)k; }
}
// Called twice: before the SDK starts (late = 0) and after nninitStartup and the module constructors, where the job runs (late = 1). Oodle is served at the first: it takes its work memory from the
// host. LZ5 at the second: LZ5_decompress_safe takes 512 KB of work memory from the game's malloc on every call, a StandardAllocator that nninitStartup sets up (DRAGON QUEST XI S main+0x4530440),
// and returns -1 before that
void dm_oodle_serve_maybe(int late) {
    if (!getenv("DM_OODLE_SERVE")) return;
    dec_find("DM_OODLE_SERVE");
    if (late != (g_dec.lz5 != NULL)) return;
    enum { SLACK = 0x10000 };
    size_t scratch_n = 16u << 20; uint8_t *scratch = malloc(scratch_n), *comp = NULL, *raw = NULL; size_t comp_cap = 0, raw_cap = 0; uint64_t n = 0, bad = 0;   // the decoder's work memory, as for a job
    if (!scratch) die("DM_OODLE_SERVE: decoder memory");
    LOG("DM_OODLE_SERVE: %s at main+%#llx, requests on stdin", g_dec.name, (unsigned long long)g_dec.off);
    for (;;) {
        uint64_t hdr[2];
        if (!read_all(0, hdr, sizeof hdr)) break;
        if (hdr[0] > (1ull << 32) || hdr[1] > (1ull << 32)) die("DM_OODLE_SERVE: request too large (%#llx → %#llx)", (unsigned long long)hdr[0], (unsigned long long)hdr[1]);
        if (hdr[0] > comp_cap) { free(comp); comp_cap = hdr[0]; if (!(comp = malloc(comp_cap))) die("DM_OODLE_SERVE: input buffer"); }
        if (hdr[1] + SLACK > raw_cap) { free(raw); raw_cap = hdr[1] + SLACK; if (!(raw = malloc(raw_cap))) die("DM_OODLE_SERVE: output buffer"); }
        if (!read_all(0, comp, hdr[0])) die("DM_OODLE_SERVE: request cut short");
        int64_t got = dec_run(comp, (int64_t)hdr[0], raw, (int64_t)hdr[1], scratch, (int64_t)scratch_n);
        uint64_t out = got == (int64_t)hdr[1] ? (uint64_t)got : 0;
        if (!out && hdr[1] && bad++ < 8) LOG("[RED] DM_OODLE_SERVE: request %llu (%llu → %llu bytes) failed (decoder returned %lld)", (unsigned long long)n, (unsigned long long)hdr[0], (unsigned long long)hdr[1], (long long)got);
        write_all(1, &out, sizeof out);
        if (out) write_all(1, raw, out);
        n++;
    }
    LOG("DM_OODLE_SERVE: %llu requests, %llu failed", (unsigned long long)n, (unsigned long long)bad);
    fflush(stderr); _exit(bad ? 1 : 0);
}
