// An NVN shader's control section, NVN's or the Switch 2's NVN2 (docs/ns2-port.md): what the runtime reads of it.
// NVN (Switch 1): 0x880 bytes (BotW's own sizes at +0x20); +0x700 the c1 segment's start in the code area, +0x704 the code
// area's length, +0x714 the stage, +0x718 the fragment flags, +0x748 / +0x754 compute's thread group and shared memory. The
// code area starts with a 0x30-byte header (magic 0x12345678 graphics, 0x12345679 compute), then the 0x50-byte SPH.
// NVN2 (magic 0x98761234, version 0xc9; read from seedds/nxnative's dmsh_ampere.py and shader_split.py, MIT): +0x38 the
// instructions' length (the 0x80-byte SPH v4 of the graphics stages included), +0x40 that rounded up to 0x100, +0x44 the
// code area's length, the c1 segment lying between the two; +0x56 the stage byte (5 compute); +0x58 / +0x64 compute's
// thread group (3 x u32) and shared memory; +0xa9 compute's push-constant words, +0xb1..+0xb3 its uniform buffer, storage
// buffer and texture entry counts and +0xba (u16) its c[0] size. The code area has no header: a graphics stage starts with
// its SPH, whose word 0 bits 10..13 give the shader type.
#pragma once
#include <CommonCrypto/CommonDigest.h>
#include <stdint.h>
#include <string.h>

static inline int nvn2_ctl(const uint8_t *c) {
    uint32_t magic, version; memcpy(&magic, c, 4); memcpy(&version, c + 4, 4);
    return magic == 0x98761234u && version == 0xc9;
}
// The code area's length (the key hashes that much code).
static inline uint32_t ctl_code_len(const uint8_t *c) { uint32_t n; memcpy(&n, c + (nvn2_ctl(c) ? 0x44 : 0x704), 4); return n; }
// The NVN stage (0 vertex, 1 fragment, 2 geometry, 3 tessellation control, 4 tessellation evaluation, 5 compute); NVN2 takes
// the graphics stages from the SPH at the code's start (shader type 1 vertex, 2 / 3 tessellation, 4 geometry, 5 pixel; 0 is
// read as vertex, as Ryujinx's ShaderHeader does). -1: not a stage.
static inline int ctl_stage(const uint8_t *c, const uint8_t *code) {
    if (!nvn2_ctl(c)) { uint32_t st; memcpy(&st, c + 0x714, 4); return st <= 5 ? (int)st : -1; }
    if (c[0x56] == 5) return 5;
    if (c[0x56] == 6) return 3;
    if (c[0x56] == 7) return 4;
    uint32_t w0; memcpy(&w0, code, 4);
    static const int from_sph[6] = {0, 0, 3, 4, 2, 1};
    unsigned type = (w0 >> 10) & 0xf;
    return (w0 & 0x1f) && type <= 5 ? from_sph[type] : -1;
}
// The SPH's word 0 (the graphics stages): after the code area's 0x30-byte header in NVN, at its start in NVN2.
static inline uint32_t ctl_sph_word0(const uint8_t *c, const uint8_t *code) { uint32_t w; memcpy(&w, code + (nvn2_ctl(c) ? 0 : 0x30), 4); return w; }
// The control section's part of the shader key. NVN: its own size (the word at +0x20: 0x880 in TOTK's archives and agl
// packages; 0x7d1 and 0x848 in BotW, whose sections live in their own allocations), never more than 0x880. NVN2: the first
// 0x48 bytes (through the code lengths) with the words a game holds otherwise in memory than in its files hashed as zero, none
// of which the translation reads (totk_shader_rt.m variant_request): +0x30 (BNSH files hold the section's length there,
// agl's BAHS and memory 0) and, since 2026-11-12, +0x08..+0x0b and the shader hash at +0x20..+0x27, which Pokopia writes
// again before nvnProgramSetShaders (112 of the 309 shaders it set by its title screen had a game file's code and a key of
// their own, so 106 the seed held were translated again in play). ns2_shader_corpus.py shader_key hashes the same.
// The NVN2 key rule's name: a Switch 2 seed records the one its keys were made with (prepare_ns2_seed.py shader_key_rule),
// and update_runtime.py makes the seed again when the runtime's is another (its keys would name no shader the runtime asks for).
// A new rule takes a new name here and in ns2_shader_corpus.py KEY_RULE (tests/test_ns2_key_rule.py checks they are equal).
// The rule is the game's: Ocarina of Time keeps its own (only +0x30 as zero; games/oot/scripts/ns2_shader_corpus.py), which
// names its shader list, its seeds and its players' contributions. The shared layer gave it the rule above for a while
// (2026-11-13): its seed named 0 of the list's 8,411 shaders. Readers take a game's line by its tag (update_runtime.py).
#if defined(GAME_OOT)
#define NVN2_KEY_RULE "nvn2-ctl48-zero-30"   /* game: oot */
#define NVN2_KEY_ZERO_08_20 0
#else
#define NVN2_KEY_RULE "nvn2-ctl48-zero-08-20-30"   /* game: totk (Donkey Kong Bananza and Pokemon Pokopia, built with games/totk) */
#define NVN2_KEY_ZERO_08_20 1
#endif
static inline void ctl_key_update(CC_SHA256_CTX *ctx, const uint8_t *c) {
    if (nvn2_ctl(c)) {
        uint8_t k[0x48]; memcpy(k, c, sizeof k);
        if (NVN2_KEY_ZERO_08_20) { memset(k + 0x08, 0, 4); memset(k + 0x20, 0, 8); }
        memset(k + 0x30, 0, 4);
        CC_SHA256_Update(ctx, k, sizeof k); return;
    }
    uint32_t n; memcpy(&n, c + 0x20, 4);
    CC_SHA256_Update(ctx, c, n >= 0x7cc && n < 0x880 ? n : 0x880);
}
