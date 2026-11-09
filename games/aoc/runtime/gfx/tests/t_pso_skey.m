// Serializable pipeline key unit tests: the pso_skey hex round trip is byte-identical; tampering / wrong length / illegal characters are detected (expectations given directly by construction).
// Normalized keys (nvn_pso_norm.h): rows that differ only in colour state the pipeline does not use parse to one key, and the warm-up's row counts add up.
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdlib.h>
#include "../nvn/nvn_obj.h"
@interface NvShader : NSObject { @public int stage; char key[24]; } @end
@implementation NvShader @end
#include "../nvn/nvn_pso_skey.h"

static void fill(pso_key *k, unsigned seed) {
    memset(k, 0, sizeof *k);
    for (size_t i = 0; i < sizeof *k; i++) ((unsigned char *)k)[i] = (unsigned char)(seed + i * 13);
    k->vs = (void *)1; k->fs = (void *)2; k->fsfn = (void *)3;
}

static void t_norm(void) {
    pso_key a; memset(&a, 0, sizeof a); a.cfmt[0] = 70; a.blend_en = 0x9; a.chmask = 0xff;   // target 0 present and blended, target 1 absent, target 3 absent and blended
    a.blend[0].src_rgb = 4; a.blend[1].src_rgb = 5; a.blend[3].src_rgb = 7;
    pso_key n = a; pso_key_norm(&n, 0);
    assert(n.blend_en == 0x1 && n.chmask == 0x0f && n.blend[0].src_rgb == 4 && !n.blend[1].src_rgb && !n.blend[3].src_rgb);   // absent targets lose blend and mask
    pso_key m = a; pso_key_norm(&m, 1);
    assert(m.blend_en == 0x9 && m.chmask == 0xff && m.blend[3].src_rgb == 7 && !m.blend[1].src_rgb);   // a merged pass keeps them (lmap), only unblended factors go
    pso_key b = a; b.chmask = 0x5f; b.blend[1].src_rgb = 6; b.blend[2].dst_a = 3;   // differs from a only where the pipeline does not look
    pso_skey sa, sb, pa, pb; char ha[sizeof sa * 2 + 1], hb[sizeof sb * 2 + 1];
    pso_skey_from(&a, nil, nil, 0, NULL, &sa); pso_skey_hex(&sa, ha); pso_skey_from(&b, nil, nil, 0, NULL, &sb); pso_skey_hex(&sb, hb);
    assert(strcmp(ha, hb) != 0); assert(pso_skey_parse(ha, &pa) == 0 && pso_skey_parse(hb, &pb) == 0);
    assert(memcmp(&pa, &pb, sizeof pa) == 0 && memcmp(&pa.k, &n, sizeof n) == 0);   // two old rows, one pipeline: the key pso_key_fill makes now
    pso_warm_rows c = {.rows = 10, .dup = 2, .unreadable = 1, .no_shader = 1, .no_fz = 1, .reached = 5, .calls = 6, .retries = 1, .made = 4, .failed = 1};
    char line[512]; assert(pso_warm_rows_ok(&c)); pso_warm_rows_line(&c, "seed", line, sizeof line);
    assert(strstr(line, "10 = 2 duplicate rows avoided + 3 skipped before Metal") && strstr(line, "6 Metal calls (1 layer retries)") && !strstr(line, "[RED]"));
    c.made = 5; assert(!pso_warm_rows_ok(&c)); pso_warm_rows_line(&c, "seed", line, sizeof line); assert(strstr(line, "[RED]"));
}

int main(void) {
    t_norm();
    for (unsigned seed = 1; seed <= 3; seed++) {
        pso_key k; fill(&k, seed); pso_key_norm(&k, seed & 1);   // a live key is normalized (pso_key_fill), so its row round-trips
        NvShader *vs = [NvShader new], *fs = [NvShader new];
        vs->stage = (int)seed; fs->stage = (int)(seed + 1);
        snprintf(vs->key, sizeof vs->key, "vs-%u", seed); snprintf(fs->key, sizeof fs->key, "fs-%u", seed);
        pso_fz_desc fz = {.kind = 1, .fbmask = 0xa5};
        for (int i = 0; i < 32; i++) fz.fx[i] = (int8_t)(i - 7);
        pso_skey a, b; char hex[sizeof a * 2 + 1];
        pso_skey_from(&k, vs, fs, seed & 1, &fz, &a); pso_skey_hex(&a, hex);
        assert(pso_skey_parse(hex, &b) == 0); assert(memcmp(&a, &b, sizeof a) == 0);
        hex[0] = hex[0] == '0' ? '1' : '0'; assert(pso_skey_parse(hex, &b) == 0); assert(memcmp(&a, &b, sizeof a) != 0);
        assert(pso_skey_parse(hex + 1, &b) == -1); hex[0] = 'g'; assert(pso_skey_parse(hex, &b) == -1);
    }
    puts("PSO_SKEY OK 3 cases round-trip; one changed character differs, missing digits / illegal characters rejected; unused colour state normalized, row counts add up");
    return 0;
}
