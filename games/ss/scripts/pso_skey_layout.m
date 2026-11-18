// Prints the layout of the host's pipeline manifest key (pso_skey, runtime/gfx/nvn/nvn_pso_skey.h) for pso_seed.py:
// size, then the offsets of the vertex key, fragment key, vertex stage, fragment stage, merged and fz bytes.
#import <Foundation/Foundation.h>
#include <stddef.h>
#include <stdio.h>
#include "nvn_obj.h"
@interface NvShader : NSObject { @public int stage; char key[24]; } @end   // as runtime/gfx/tests/t_pso_skey.m: the header only names it
@implementation NvShader @end
#include "nvn_pso_skey.h"

int main(void) {
    printf("%zu %zu %zu %zu %zu %zu %zu %zu\n", sizeof(pso_skey), offsetof(pso_skey, vs), offsetof(pso_skey, fs), sizeof(((pso_skey *)0)->vs),
           offsetof(pso_skey, vs_stage), offsetof(pso_skey, fs_stage), offsetof(pso_skey, merged), offsetof(pso_skey, fz));
    return 0;
}
