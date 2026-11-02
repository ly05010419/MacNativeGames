// Serializable pipeline key unit tests: the pso_skey hex round trip is byte-identical; tampering / wrong length / illegal characters are detected (expectations given directly by construction).
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

int main(void) {
    for (unsigned seed = 1; seed <= 3; seed++) {
        pso_key k; fill(&k, seed); pso_key_norm(&k, seed & 1);   // keys are made normalized (pso_key_fill), and a parse normalizes
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
    // pso_key_norm: blend factors of a target that does not blend, and the mask / blend bit of a target a normal pass lacks, are not part of the key; a merged pass keeps
    // the mask and blend bits (they index logical targets through lmap)
    {
        NvShader *vs = [NvShader new]; snprintf(vs->key, sizeof vs->key, "vs-n");
        pso_key k1; memset(&k1, 0, sizeof k1); k1.cfmt[0] = 80; k1.cfmt[1] = 80; k1.blend_en = 1; k1.chmask = 0xff;
        k1.blend[0].src_rgb = 1; k1.blend[0].dst_rgb = 2;
        pso_key k2 = k1; k2.blend[1].src_rgb = 7; k2.blend[3].dst_a = 5; k2.blend_en |= 1u << 5; k2.chmask |= 0xfu << 20;
        pso_skey a, b; char ha[sizeof a * 2 + 1], hb[sizeof b * 2 + 1];
        pso_skey_from(&k1, vs, nil, 0, NULL, &a); pso_skey_from(&k2, vs, nil, 0, NULL, &b); assert(memcmp(&a, &b, sizeof a) != 0);
        pso_skey_hex(&a, ha); pso_skey_hex(&b, hb); assert(pso_skey_parse(ha, &a) == 0 && pso_skey_parse(hb, &b) == 0);
        assert(memcmp(&a, &b, sizeof a) == 0 && a.k.blend[0].src_rgb == 1 && a.k.blend[0].dst_rgb == 2 && a.k.chmask == 0xff && a.k.blend_en == 1);
        pso_skey_from(&k2, vs, nil, 1, NULL, &b); pso_skey_hex(&b, hb); assert(pso_skey_parse(hb, &b) == 0);
        assert(b.k.chmask == (0xffu | 0xfu << 20) && b.k.blend_en == (1u | 1u << 5) && b.k.blend[1].src_rgb == 0 && b.k.blend[3].dst_a == 0);
    }
    puts("PSO_SKEY OK 3 cases round-trip; one changed character differs, missing digits / illegal characters rejected; unused blend and mask state left out of the key");
    return 0;
}
