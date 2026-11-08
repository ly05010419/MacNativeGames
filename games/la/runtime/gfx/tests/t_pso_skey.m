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
        pso_key k; fill(&k, seed);
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
    puts("PSO_SKEY OK 3 cases round-trip; one changed character differs, missing digits / illegal characters rejected");
    return 0;
}
