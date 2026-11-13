/* 渲染分辨率：把 main 里 15 处尺寸立即数在 1080p / 1440p / 2160p 三档之间切换，选一次就换一个渲染尺寸，不必重新改写
   镜像、也不必重打 app。镜像本身按 1440p 打包（patch_nso.py 的 ENGAGE_RES_CUTS，ENGAGE_OUTPUT_RES_2K=1 时的结果），
   这里能写回 1080p 的原字，也能在 1080p 镜像上写 1440p，2160p 则由 1440p 那条指令**算**出来。

   Render resolution: the 15 size immediates in main are switched between 1080p, 1440p and 2160p at startup, so one app can
   offer all three from the F1 panel instead of a rewrite per size. The shipped image carries the 1440p words (patch_nso.py's
   ENGAGE_RES_CUTS, applied with ENGAGE_OUTPUT_RES_2K=1); the 1080p words are the game's originals, and the 2160p words are
   derived from the 1440p one (same rd, imm 3840 for a width site / 2160 for a height site).

   Safety. The offsets and the two measured words come from third_party/d2r/patch_nso.py (ENGAGE_RES_CUTS): for every site
   the table keeps what that word does and whether it is the width or the height. Writing happens only when the module's GNU
   build-id is the one this table was measured on, and each word is read back first: it must be one of the three known words,
   otherwise that one site is skipped with a red line instead of being written blind. A site whose 1440p word cannot be
   decoded as a single "movz wRd,#imm" (or whose measured imm does not match its width/height axis) is never guessed at: in
   the 2160p tier it stays at the 1440p value and the log says so, per site. */
#include "runtime.h"
#include "render_res.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The build the offsets were measured on: main.cuts.tsv's "# BUILD_ID" line, i.e. the NSO header's module id. */
#define RENDER_RES_BUILD_ID "8c08b9719e085f91847b5e0f935d9488"

enum { SITE_WIDTH, SITE_HEIGHT };
/* offset: main's image offset (the load address is added at run time); insn_1080 / insn_1440: the little-endian words as
   patch_nso.py's ENGAGE_RES_CUTS has them ('08878052' → 0x52808708), 1080p being the game's original; axis: which of the
   two numbers that word carries (decoded and checked at run time, see check_axis). */
typedef struct { uint32_t offset, insn_1080, insn_1440; unsigned char axis; const char *what; } ResCut;
static const ResCut res_cuts[] = {
    {0x4b0158, 0x52808708u, 0x5280b408u, SITE_HEIGHT,   /* '08878052' / '08b48052' */
     "显示对象的高上限 movz w8,#1080 → #1440（vtable +0x28 的夹取，写 +0xc4） | display object height cap (clamped into +0xc4)"},
    {0x4b0168, 0x5280f009u, 0x52814009u, SITE_WIDTH,    /* '09f08052' / '09408152' */
     "显示对象的宽上限 movz w9,#1920 → #2560（同函数，写 +0xc0） | display object width cap (same function, stores +0xc0)"},
    {0x4b0230, 0xb940c000u, 0x52814000u, SITE_WIDTH,    /* '00c040b9' / '00408152' */
     "Screen 宽度 getter：ldr w0,[x0,#0xc0] → movz w0,#2560（vtable +0xa8；Screen.width 与 Display 的尺寸都走它）"
     " | Screen width getter (vtable +0xa8; Screen.width and the Display extents)"},
    {0x4b0238, 0xb940c400u, 0x5280b400u, SITE_HEIGHT,   /* '00c440b9' / '00b48052' */
     "Screen 高度 getter：ldr w0,[x0,#0xc4] → movz w0,#1440（vtable +0xb0） | Screen height getter (vtable +0xb0)"},
    {0xa67068, 0x5280f008u, 0x52814008u, SITE_WIDTH,    /* '08f08052' / '08408152' */
     "GetDisplaySize 的宽（csel 常量分支）：经 x20/x19 出参返回，交换链与 WindowCrop 共用"
     " | GetDisplaySize width (the csel constant branch; the swap chain and WindowCrop share it)"},
    {0xa67070, 0x52808709u, 0x5280b409u, SITE_HEIGHT,   /* '09878052' / '09b48052' */
     "GetDisplaySize 的高 | GetDisplaySize height"},
    {0xa67080, 0x5280f008u, 0x52814008u, SITE_WIDTH,    /* '08f08052' / '08408152' */
     "GetDisplaySize 的宽（另一分支 / 回落） | GetDisplaySize width (the other branch / fallback)"},
    {0xa67088, 0x52808708u, 0x5280b408u, SITE_HEIGHT,   /* '08878052' / '08b48052' */
     "GetDisplaySize 的高（另一分支 / 回落） | GetDisplaySize height (the other branch / fallback)"},
    {0xa7cff0, 0x52808715u, 0x5280b415u, SITE_HEIGHT,   /* '15878052' / '15b48052' */
     "交换链贴图 1/3 的高（同函数后续用 mov w2,w21 复用） | swap-chain texture 1/3 height (reused later as w2)"},
    {0xa7cff8, 0x5280f001u, 0x52814001u, SITE_WIDTH,    /* '01f08052' / '01408152' */
     "交换链贴图 1/3 的宽 | swap-chain texture 1/3 width"},
    {0xa7d08c, 0x5280f001u, 0x52814001u, SITE_WIDTH,    /* '01f08052' / '01408152' */
     "交换链贴图 2/3 的宽（高复用 w21） | swap-chain texture 2/3 width (height reuses w21)"},
    {0xa7d26c, 0x5280f001u, 0x52814001u, SITE_WIDTH,    /* '01f08052' / '01408152' */
     "交换链贴图 3/3 的宽 | swap-chain texture 3/3 width"},
    {0xa7d270, 0x52808702u, 0x5280b402u, SITE_HEIGHT,   /* '02878052' / '02b48052' */
     "交换链贴图 3/3 的高 | swap-chain texture 3/3 height"},
    {0xa65e70, 0x52808703u, 0x5280b403u, SITE_HEIGHT,   /* '03878052' / '03b48052' */
     "界面/画布渲染目标的请求高（0xa65e98 调槽 0x3e8；不改则 UI 画在 1080p 目标上，1:1 贴到 1440p 帧里出现黑边）"
     " | UI/canvas render target request height (the 0xa65e98 call; leaving it at 1080p letterboxes the menu frames)"},
    {0xa65e7c, 0x5280f002u, 0x52814002u, SITE_WIDTH,    /* '02f08052' / '02408152' */
     "界面/画布渲染目标的请求宽 | UI/canvas render target request width"},
};

/* 每档该写的宽/高：1080p 与 1440p 的字来自镜像实测，2160p 由 1440p 那条算出来（见 word_for）。
   The width/height each tier asks for. The 1080p and 1440p words are measured (patch_nso.py); 2160p is derived. */
static const uint32_t tier_width[3] = {1920u, 2560u, 3840u};
static const uint32_t tier_height[3] = {1080u, 1440u, 2160u};

/* movz wRd,#imm：0x52800000 | ((imm & 0xffff) << 5) | Rd。按这个公式算而不是手抄：算出来的 1440p 字必须与表里实测的
   那条完全一致，apply 时的逐条校验就是这么自检的。
   "movz wRd,#imm" as the architecture encodes it, so the derived 2160p words cannot drift from a typo: the 1440p word this
   formula gives has to match the measured one, which the per-site check below enforces. */
static int movz_word(uint32_t rd, uint32_t imm, uint32_t *out) {
    if (imm > 0xFFFFu) return 0;
    *out = 0x52800000u | ((imm & 0xFFFFu) << 5) | (rd & 0x1Fu);
    return 1;
}
static int movz_decode(uint32_t word, uint32_t *rd, uint32_t *imm) {
    if ((word & 0xFF800000u) != 0x52800000u) return 0;   /* 32-bit movz, hw = 0 | 32-bit "movz w" with shift 0 */
    *rd = word & 0x1Fu;
    *imm = (word >> 5) & 0xFFFFu;
    return 1;
}
/* 一条站点在某档该写的字；2160p 无法用单条 movz 表达时返回 0（该条保持 1440p）。
   The word a site takes in a tier; 0 when the 2160p tier cannot be expressed as one movz (the site then keeps 1440p). */
static int word_for(const ResCut *c, int tier, uint32_t *out) {
    if (tier == 0) { *out = c->insn_1080; return 1; }
    if (tier == 1) { *out = c->insn_1440; return 1; }
    uint32_t rd, imm;
    if (!movz_decode(c->insn_1440, &rd, &imm)) return 0;
    return movz_word(rd, c->axis == SITE_WIDTH ? tier_width[2] : tier_height[2], out);
}
/* 表里的宽/高标注与实测指令是否一致：1440p 那条的立即数必须是 2560（宽）或 1440（高）。不一致就是表抄错了，该条不写。
   Whether the measured word agrees with the width/height the table claims: 2560 for a width site, 1440 for a height one.
   A mismatch means the table is wrong for this build, so that site is left alone. */
static const char *axis_mismatch(const ResCut *c) {
    uint32_t rd, imm;
    if (!movz_decode(c->insn_1440, &rd, &imm)) return "1440p 的字不是单条 movz，无法推出 2160p";
    uint32_t expect = c->axis == SITE_WIDTH ? tier_width[1] : tier_height[1];
    if (imm != expect) return "表里标的宽/高与镜像里的立即数不符";
    if (movz_decode(c->insn_1080, &rd, &imm)) {
        uint32_t base = c->axis == SITE_WIDTH ? tier_width[0] : tier_height[0];
        if (imm != base) return "表里标的宽/高与 1080p 的立即数不符";
    }
    return NULL;
}

/* 0 = 1080p, 1 = 1440p（默认，镜像现状）, 2 = 2160p。设置与 F1 面板同一个 defaults 域：测试档（TOTK_SLOT_OWNER 不是
   user）写 ...display.tests，玩家档写 ...display。
   0 = 1080p, 1 = 1440p (default: what the rewritten image carries), 2 = 2160p, read from the same defaults suite as the
   F1 panel: the test suite when TOTK_SLOT_OWNER says so, the player suite otherwise. */
static int render_res_wanted(void) {
    const char *owner = getenv("TOTK_SLOT_OWNER");
    const char *name = owner && *owner && strcmp(owner, "user") ? "org.macnativegames.totk.display.tests"
                                                               : "org.macnativegames.totk.display";
    CFStringRef suite = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    if (!suite) return 1;
    CFPropertyListRef value = CFPreferencesCopyAppValue(CFSTR("renderResolution"), suite);
    CFRelease(suite);
    int want = 1;
    if (value) {
        if (CFGetTypeID(value) == CFNumberGetTypeID()) {
            CFIndex n = 1;
            CFNumberGetValue(value, kCFNumberCFIndexType, &n);
            want = n <= 0 ? 0 : n >= 2 ? 2 : 1;
        } else if (CFGetTypeID(value) == CFBooleanGetTypeID()) want = CFBooleanGetValue(value) ? 1 : 0;
        CFRelease(value);
    }
    return want;
}

/* main 的 GNU build-id（.note.gnu.build-id：namesz 4 / descsz 16 / type 3 / "GNU\0" + 16 字节），找不到就给空串。
   The image's GNU build-id note, or an empty string: the NSO header's module id (what patch_nso.py keys its cut tables
   on) is not in the image, but the linker also put the same 16 bytes in the note section. */
void runtime_render_res_build_id(const void *image, size_t size, char out[33]) {
    static const unsigned char note[12] = {4, 0, 0, 0, 16, 0, 0, 0, 3, 0, 0, 0};   /* Elf_Nhdr: namesz 4, descsz 16, type 3 */
    out[0] = 0;
    if (!image) return;
    const unsigned char *bytes = image;
    for (size_t at = 0; at + 32 <= size;) {
        const unsigned char *g = memchr(bytes + at, 'G', size - at - 31);
        if (!g) return;
        size_t hit = (size_t)(g - bytes);
        if (hit >= 12 && hit + 19 <= size && !memcmp(bytes + hit - 12, note, sizeof note) && !memcmp(bytes + hit, "GNU", 3) &&
            bytes[hit + 3] == 0) {
            for (unsigned i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", bytes[hit + 4 + i]);
            return;
        }
        at = hit + 1;
    }
}

void runtime_render_res_apply(void *main_base, const char *build_id) {
    if (!main_base) {
        fprintf(stderr, "[totk-render] 【红】没有 main 的基址，渲染分辨率保持镜像原样\n");
        return;
    }
    if (!build_id || strcmp(build_id, RENDER_RES_BUILD_ID)) {
        fprintf(stderr, "[totk-render] main 的 build id 是 %s，不是登记过的 %s：渲染分辨率保持镜像原样\n",
                build_id && *build_id ? build_id : "(未找到)", RENDER_RES_BUILD_ID);
        return;
    }
    const int tier = render_res_wanted();
    unsigned char *base = main_base;
    const unsigned sites = sizeof res_cuts / sizeof *res_cuts;
    unsigned applied = 0, skipped = 0, held = 0, held_at[8], skipped_at[8];
    const char *held_why[8];
    for (unsigned i = 0; i < sites; i++) {
        const ResCut *c = &res_cuts[i];
        uint32_t want = 0, other_lo = 0, other_hi = 0;
        const char *mismatch = axis_mismatch(c);
        int expressible = word_for(c, tier, &want);
        if (tier == 2 && expressible && mismatch) expressible = 0;   /* a wrong axis claim must not be turned into a write */
        uint32_t now;
        memcpy(&now, base + c->offset, sizeof now);
        if (!expressible) {
            /* 2160p 写不了这一条：写回 1440p 的值，让"保持 1440p"是真的，而不是碰巧镜像就是 1440p。 */
            uint32_t keep = c->insn_1440;
            if (now != keep && (now == c->insn_1080 || now == c->insn_1440)) {
                memcpy(base + c->offset, &keep, sizeof keep);
                __builtin___clear_cache((char *)base + c->offset, (char *)base + c->offset + sizeof keep);
            }
            if (held < sizeof held_at / sizeof *held_at) {
                held_at[held] = c->offset;
                held_why[held] = mismatch ? mismatch : "2160p 无法用单条 movz 表达";
                held++;
            }
            continue;
        }
        word_for(c, 0, &other_lo);
        word_for(c, 1, &other_hi);
        if (now == want) { applied++; continue; }
        if (now != other_lo && now != other_hi) {
            fprintf(stderr, "[totk-render] 【红】main+%#x 是 %08x，不是登记的三档字之一，跳过：%s\n",
                    (unsigned)c->offset, now, c->what);
            if (skipped < sizeof skipped_at / sizeof *skipped_at) skipped_at[skipped++] = c->offset;
            continue;
        }
        memcpy(base + c->offset, &want, sizeof want);
        __builtin___clear_cache((char *)base + c->offset, (char *)base + c->offset + sizeof want);
        applied++;
    }
    if (!held && !skipped) {
        fprintf(stderr, "[totk-render] 渲染分辨率 %dp（%u 条立即数已应用）\n", tier_height[tier], applied);
    } else {
        fprintf(stderr, "[totk-render] 渲染分辨率 %dp（%u 条中 %u 条已应用", tier_height[tier], sites, applied);
        for (unsigned i = 0; i < held; i++) fprintf(stderr, "，%u 条保持 1440p：main+%#x %s", held, held_at[i], held_why[i]);
        for (unsigned i = 0; i < skipped; i++) fprintf(stderr, "，main+%#x 校验不符已跳过", skipped_at[i]);
        fprintf(stderr, "）\n");
    }
}
